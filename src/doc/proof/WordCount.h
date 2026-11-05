#pragma once

#include <cstdint>

#include "doc/model/Document.h"

// Word count (plan §6.2, milestone D2), counted as Word counts: a word is a run of characters
// between spaces; characters are counted with and without spaces; empty paragraphs don't count.
// Field codes, deleted text and hidden text aren't counted; field results are.
namespace fw::doc::proof {

struct Counts {
  uint64_t words = 0;
  uint64_t characters = 0;        // without spaces
  uint64_t charactersSpaces = 0;  // with spaces
  uint64_t paragraphs = 0;
  Counts& operator+=(const Counts& o) {
    words += o.words;
    characters += o.characters;
    charactersSpaces += o.charactersSpaces;
    paragraphs += o.paragraphs;
    return *this;
  }
};

// One paragraph, or the part of it in [from, to).
Counts CountParagraph(const Document& doc, uint32_t para, uint32_t from = 0, uint32_t to = UINT32_MAX);
// The main story, and with `notesAndBoxes` the footnotes, endnotes and text boxes too (Word's
// "Include textboxes, footnotes and endnotes").
Counts CountDocument(const Document& doc, bool notesAndBoxes);
// [from, to) in one story.
Counts CountRange(const Document& doc, Pos from, Pos to);

}  // namespace fw::doc::proof
