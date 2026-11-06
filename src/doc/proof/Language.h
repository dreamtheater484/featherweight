#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "doc/edit/Editor.h"
#include "doc/model/Document.h"

// The language of text in a document (w:lang), for spelling and the status bar (plan §6.2,
// milestone D2). A character takes its run's w:lang, w:eastAsia or w:bidi by its script; a run
// without one takes the document's theme language (w:themeFontLang), then the PC's own language,
// as Word does.
namespace fw::doc::proof {

// True for characters Word treats as East Asian (Han, kana, Hangul, East Asian punctuation and
// full-width forms).
bool IsEastAsian(char32_t c);

// The language of the run `run` in `para` for a character `c` of it.
std::wstring LanguageOf(const Document& doc, uint32_t para, Node run, char16_t c);
// The language at a position: the character after it, or before it at the end of a paragraph.
std::wstring LanguageAt(const Document& doc, Pos pos);
// The language of the text in [from, to) (one story), or nullopt when it mixes languages. An
// empty range gives the language at `from`.
std::optional<std::wstring> LanguageOfRange(const Document& doc, Pos from, Pos to);

// The language's name in the user's language ("English (United States)"), or the tag itself.
std::wstring LanguageName(std::wstring_view tag);

// Sets w:lang (the language of Latin text) on [from, to), as Word's Set Proofing Language does.
// One undo step, named "Language".
edit::Result SetLanguage(edit::Editor& editor, Pos from, Pos to, std::wstring_view tag);

}  // namespace fw::doc::proof
