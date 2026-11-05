#pragma once

#include <optional>
#include <string>
#include <vector>

#include "doc/edit/Editor.h"

// AutoCorrect (plan §6.2, milestone D2): Word's default rules and its common replacements, made
// as the user types. Each correction is an undo step of its own ("AutoCorrect"), right after the
// typing, so one Ctrl+Z puts back exactly what was typed.
//
// Rules, as Word applies them by default:
// - Replace text as you type: "(c)" © "(r)" ® "(tm)" ™ "(e)" € "..." … arrows, and common
//   misspellings ("teh" the), keeping the word's capitals. The misspelling list is English, used
//   for English text; the symbols apply in every language.
// - Straight quotes to smart quotes (‘ ’ “ ”; „ “ for German).
// - Hyphens to dashes: "word--word" gives an em dash, "word - word" an en dash.
// - Correct TWo INitial CApitals; capitalize the first letter of sentences; capitalize the
//   names of days (English).
namespace fw::doc::proof {

struct AutoCorrectOptions {
  bool replaceText = true;
  bool smartQuotes = true;
  bool dashes = true;
  bool twoInitialCapitals = true;
  bool capitalizeSentences = true;
  bool capitalizeDays = true;
};

// A correction in one paragraph: [from, to) becomes `text`.
struct Correction {
  uint32_t from = 0, to = 0;
  std::u16string text;
};

class AutoCorrect {
 public:
  explicit AutoCorrect(AutoCorrectOptions options = {}) : options_(options) {}
  AutoCorrectOptions& Options() { return options_; }

  // After the user typed `typed`, the character just before `caret`. Makes the corrections it
  // completes as one undo step, and returns where the caret goes then; nullopt when nothing
  // changed.
  std::optional<Pos> AfterTyping(edit::Editor& editor, Pos caret, char16_t typed);
  // After Enter split a paragraph: the word that ended `para` is complete.
  std::optional<Pos> AfterParagraphBreak(edit::Editor& editor, uint32_t para);

  // The corrections, without making them: for `text` (one paragraph) with the caret at `caret`
  // just after the typed character, in language `lang` (a BCP 47 tag). Right to left, so each
  // can be made without moving the others. `typed` 0 means the paragraph ended at `caret`.
  std::vector<Correction> Find(std::u16string_view text, uint32_t caret, char16_t typed, std::wstring_view lang) const;

 private:
  std::optional<Pos> Apply(edit::Editor& editor, uint32_t para, uint32_t caret, std::vector<Correction> corrections);

  AutoCorrectOptions options_;
};

}  // namespace fw::doc::proof
