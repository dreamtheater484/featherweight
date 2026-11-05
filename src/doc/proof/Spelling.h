#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "doc/edit/Editor.h"
#include "doc/model/Document.h"
#include "text/SpellCheck.h"

// Spelling as you type (plan §6.2, milestone D2): every paragraph checked with Windows' spell
// checker in the language of its text, a few milliseconds at a time in idle time, the paragraphs
// on screen first. Edits mark the paragraphs they change for checking again. Once everything is
// checked nothing runs.
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
  // Changes whenever results change, for the view to redraw.
  uint32_t Revision() const { return revision_; }

  // A paragraph's errors, or nullptr when it isn't checked yet.
  const std::vector<Misspelling>* Errors(uint32_t para) const;
  // The error that covers `pos` (its end included, for a click just after a word).
  const Misspelling* At(Pos pos) const;
  // The first error after `pos` (paragraph order, wrapping around), or nullopt when none is known.
  std::optional<std::pair<uint32_t, Misspelling>> Next(Pos pos) const;
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
