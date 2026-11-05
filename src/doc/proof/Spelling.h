#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "doc/edit/Editor.h"
#include "doc/model/Document.h"
#include "text/SpellCheck.h"

// Spelling as you type (plan §6.2, milestone D2): paragraphs checked with Windows' spell checker
// in the language of their text, a few milliseconds at a time in idle time. The app checks the
// paragraphs on screen (Check), and those it searches through (FindNext); edits mark the
// paragraphs they change for checking again. Windows' checker takes about 12 µs a word, so a
// 300-page document would take 1.6 s: it's checked as it's read, and nothing runs once the text
// on screen is checked.
//
// Not checked: field codes, deleted and hidden text, text marked "Do not check spelling"
// (w:noProof), comments, and documents that ask for it (w:hideSpellingErrors).
namespace fw::doc::proof {

struct Misspelling {
  uint32_t from = 0, to = 0;  // [from, to) in the paragraph's text
  text::SpellChecker::Action action = text::SpellChecker::Action::Suggest;
  uint16_t lang = 0;          // Spelling::Language()
  std::wstring replacement;   // Action::Replace: the one fix
};

class Spelling {
 public:
  Spelling(const Document& doc, text::SpellChecker& checker);

  // Checks paragraphs for about `ms` milliseconds. False once all are checked.
  bool Continue(double ms);
  bool Done() const { return pending_ == 0; }
  // Checks from `para` on next (the first paragraph on screen).
  void Prefer(uint32_t para);
  // Checks those of `paras` not checked yet, for about `ms` milliseconds. True while some remain.
  bool Check(const std::vector<uint32_t>& paras, double ms);
  bool Checked(uint32_t para) const { return para < paras_.size() && paras_[para].checked; }
  // Changes whenever results change, for the view to redraw.
  uint32_t Revision() const { return revision_; }

  // A paragraph's errors, or nullptr when it isn't checked yet.
  const std::vector<Misspelling>* Errors(uint32_t para) const;
  // The error that covers `pos` (its end included, for a click just after a word).
  const Misspelling* At(Pos pos) const;
  // The first error after `pos` (paragraph order, wrapping around), or nullopt when none is known.
  std::optional<std::pair<uint32_t, Misspelling>> Next(Pos pos) const;
  // The same, checking the paragraphs on the way that aren't checked yet, for up to `ms`
  // milliseconds; `complete` is false when time ran out first (search again to go on).
  std::optional<std::pair<uint32_t, Misspelling>> FindNext(Pos pos, double ms, bool& complete);
  // The misspelled text, as the checker saw it (soft hyphens left out).
  std::wstring Word(uint32_t para, const Misspelling& m) const;
  const std::wstring& Language(const Misspelling& m) const { return langs_[m.lang]; }
  // Errors found so far.
  size_t Count() const;

  // After an edit, undo or redo: the replaced paragraphs are checked again.
  void Changed(const edit::Change& change);
  // After Ignore or Add to dictionary: paragraphs where `word` was an error are checked again.
  void Recheck(std::wstring_view word);

  // Whether the document asks Word not to show spelling errors (w:hideSpellingErrors).
  static bool HiddenByDocument(const Document& doc);

 private:
  struct ParaState {
    bool checked = false;
    std::vector<Misspelling> errors;
  };
  void CheckPara(uint32_t para);
  uint16_t LangIndex(const std::wstring& tag);

  const Document& doc_;
  text::SpellChecker& checker_;
  std::vector<ParaState> paras_;
  std::vector<std::wstring> langs_;  // installed tags the errors were found in
  size_t pending_ = 0;
  uint32_t next_ = 0;
  uint32_t revision_ = 1;
};

}  // namespace fw::doc::proof
