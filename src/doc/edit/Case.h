#pragma once

#include <string>
#include <string_view>

#include "doc/edit/Editor.h"

namespace fw::doc::edit {

// Change Case (plan §6.2 "Characters"), as Word's Aa menu and Shift+F3 do it.

enum class TextCase : uint8_t { Sentence, Lower, Upper, Capitalize, Toggle };

// Shift+F3: the case that comes next for this text, in Word's cycle: UPPERCASE after mixed text or
// Capitalize Each Word, lowercase after UPPERCASE, Capitalize Each Word after lowercase.
TextCase NextCase(std::u16string_view text);

// `text` in that case, as long as it was. `sentenceStart`: the text starts a sentence (Sentence
// case capitalizes its first letter). A word goes on through an apostrophe ("Don't").
std::u16string InCase(std::u16string_view text, TextCase c, bool sentenceStart);

// Changes the case of [from, to), in one story, keeping every run's formatting: the letters that
// change are replaced in their own run. Field codes and results, deleted text and kept content stay
// as they are. One undo step, "Change case"; the result's caret is `to`.
Result ChangeCase(Editor& ed, Pos from, Pos to, TextCase c);

}  // namespace fw::doc::edit
