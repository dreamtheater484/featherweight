#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "slide/Text.h"
#include "text/SpellCheck.h"

// Spelling as you type on slides (P2, p2-text; plan §8.2): a text body's paragraphs checked with
// Windows' spell checker (text::SpellChecker) in each run's language (a:rPr lang, as the model
// resolves it; else the user's), the way the documents' checker (src/doc/proof) splits text: East
// Asian text, fields and text marked noProof aren't checked. Results are kept by the text and its
// languages, so a body is checked again only when it changes; the tab checks the slide shown at
// idle and draws the squiggles.
namespace fw::slide::view {

struct SpellError {
  uint32_t para = 0, from = 0, to = 0;  // TextPos offsets (line breaks one character, fields their text)
  std::wstring lang;                    // the installed language it was checked in
  text::SpellChecker::Action action = text::SpellChecker::Action::Suggest;
  std::wstring replacement;             // Action::Replace
};

class SlideSpelling {
 public:
  explicit SlideSpelling(text::SpellChecker& checker) : checker_(checker) {}
  // The body's errors: from the cache when its text and languages haven't changed.
  const std::vector<SpellError>& Check(const TextBody& body);
  // After Ignore All or Add to Dictionary: everything is checked again.
  void Forget() {
    cache_.clear();
    order_.clear();
  }
  text::SpellChecker& Checker() { return checker_; }

 private:
  text::SpellChecker& checker_;
  std::unordered_map<std::wstring, std::vector<SpellError>> cache_;
  std::list<std::wstring> order_;  // oldest first, for a bounded cache
};

}  // namespace fw::slide::view
