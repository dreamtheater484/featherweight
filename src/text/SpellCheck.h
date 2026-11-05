#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Spelling with Windows' own spell checker (plan §4.4: the Spell Checking API, in every language
// the user has installed; no dictionaries ship with Featherweight). Nothing is loaded until the
// first check; then one checker per language is kept. Languages are BCP 47 tags ("en-US").
//
// The calling thread must have COM initialized (the app's UI thread does).
namespace fw::text {

class SpellChecker {
 public:
  // What the checker says about a stretch of text.
  enum class Action : uint8_t {
    Suggest,   // misspelled: ask Suggest() for replacements
    Replace,   // a known misspelling with one fix (`replacement`)
    Delete,    // a repeated word: delete it
  };
  struct Error {
    uint32_t start = 0, length = 0;  // in the checked text, UTF-16 units
    Action action = Action::Suggest;
    std::wstring replacement;  // for Replace
  };

  SpellChecker();
  ~SpellChecker();
  SpellChecker(const SpellChecker&) = delete;
  SpellChecker& operator=(const SpellChecker&) = delete;

  // False when Windows has no spell checker (before Windows 8, or the service is off).
  bool Available();
  // The language a check in `lang` uses: `lang` itself when it is installed, else an installed
  // language with the same primary language ("en-GB" when only "en-US" is installed gives
  // "en-US"), else empty: that text isn't checked.
  std::wstring Resolve(std::wstring_view lang);
  // The languages Windows can check, as installed.
  std::vector<std::wstring> Languages();

  // The errors in `text` (one paragraph, or part of one, in one language).
  std::vector<Error> Check(std::wstring_view lang, std::wstring_view text);
  // Up to `max` replacements for a misspelled word.
  std::vector<std::wstring> Suggest(std::wstring_view lang, std::wstring_view word, size_t max = 6);
  // Ignores `word` until Featherweight closes ("Ignore all").
  void Ignore(std::wstring_view lang, std::wstring_view word);
  // Adds `word` to the user's own dictionary for the language, which Windows keeps for every app
  // that uses the spell checker ("Add to dictionary").
  bool Add(std::wstring_view lang, std::wstring_view word);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fw::text
