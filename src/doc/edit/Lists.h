#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "doc/edit/Format.h"

namespace fw::doc::edit {

// Bullets and numbering (plan §6.2 "Lists"), as Word's buttons make them.
//
// A new list gets its own w:abstractNum (Word's default bullet, number or multilevel definition)
// and a w:num that names it; list counters are kept per abstract definition, so restarting is a
// new w:num with a w:startOverride, and continuing moves the items to the earlier list's w:num.
// A document without a numbering part gets one. Paragraphs in the default style take List
// Paragraph, as in Word.

enum class ListKind : uint8_t { Bullet, Number, Multilevel };

// The kind of list a w:numId shows (by its first two levels); nullopt for none.
std::optional<ListKind> KindOfList(const Document& doc, std::string_view numId);

// Word's Bullets, Numbering and Multilevel List buttons: the range's paragraphs leave the list
// when all of them are in one of this kind; otherwise they join the list of that kind just
// before them, or start a new one. Paragraphs already in a list keep their level.
void ToggleList(TreeEdit& ed, Range range, ListKind kind);

// The paragraphs leave their list. A list that comes from the paragraph style (numbered
// headings) is switched off with w:numId 0, as Word does.
void RemoveList(TreeEdit& ed, Range range);

// Restart Numbering: this item and the rest of its list count again from the level's start.
void RestartNumbering(TreeEdit& ed, uint32_t para);
// Continue Numbering: this item and the rest of its list continue the list of the same kind
// before it. False when there's none.
bool ContinueNumbering(TreeEdit& ed, uint32_t para);

// AutoFormat As You Type: what "1. ", "a) ", "(i) ", "* ", "- " or "> " typed at the start of a
// paragraph turns into. `typed` is the paragraph's text up to the caret, ending in the space or
// tab just typed.
struct AutoList {
  ListKind kind = ListKind::Number;
  std::string numFmt;   // "decimal", "lowerLetter", "upperRoman", ..., "bullet"
  std::string lvlText;  // "%1.", "(%1)", or the bullet
  std::string font;     // the bullet's font ("Symbol", "Wingdings"); empty for the text's
  int32_t start = 1;
  uint32_t length = 0;  // characters of `typed` that the label replaces
};
std::optional<AutoList> DetectAutoList(std::u16string_view typed);
// Makes the paragraph the first item of a list like that. The typed prefix is deleted by the
// caller, in the same undo step.
void StartAutoList(TreeEdit& ed, uint32_t para, const AutoList& list);

}  // namespace fw::doc::edit
