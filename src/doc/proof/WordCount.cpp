#include "doc/proof/WordCount.h"

#include <algorithm>

namespace fw::doc::proof {

namespace {

bool Deleted(Node revision) { return revision && (revision.Is(Name("del")) || revision.Is(Name("moveFrom"))); }

bool Space(char16_t c) {
  return c == u' ' || c == u'\t' || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x0E || c == 0x00A0 ||
         c == 0x2002 || c == 0x2003 || c == 0x2009 || c == 0x200B || c == 0x202F || c == 0x3000;
}

// Markers that stand for something else in the text and aren't counted as characters: note
// references, note separators, comments, fields, objects, soft hyphens.
bool Marker(char16_t c) {
  return c == 0x02 || c == 0x03 || c == 0x04 || c == 0x05 || c == 0x13 || c == 0x14 || c == 0x15 || c == 0xFFFC ||
         c == 0x00AD;
}

}  // namespace

Counts CountParagraph(const Document& doc, uint32_t para, uint32_t from, uint32_t to) {
  Counts c;
  const std::u16string& text = doc.Para(para).text;
  bool inWord = false, any = false;
  for (const TextRun& r : doc.Runs(para)) {
    if (r.fieldCode || Deleted(r.revision) || r.end <= from || r.start >= to) continue;
    if (r.run) {
      const RunProps& p = doc.ResolvedRun(para, r.run);
      if (p.vanish || p.specVanish) continue;
    }
    for (uint32_t i = std::max(r.start, from); i < std::min(r.end, to) && i < text.size(); ++i) {
      const char16_t ch = text[i];
      if (Marker(ch)) continue;
      // A surrogate pair is one character.
      if (ch >= 0xDC00 && ch <= 0xDFFF && i > 0 && text[i - 1] >= 0xD800 && text[i - 1] <= 0xDBFF) continue;
      const bool space = Space(ch);
      ++c.charactersSpaces;
      if (!space) {
        ++c.characters;
        any = true;
        if (!inWord) ++c.words;
      }
      inWord = !space;
    }
  }
  if (any) c.paragraphs = 1;
  return c;
}

Counts CountDocument(const Document& doc, bool notesAndBoxes) {
  Counts total;
  for (uint32_t s = 0; s < doc.Stories().size(); ++s) {
    const StoryKind k = doc.Stories()[s].kind;
    const bool counted = k == StoryKind::Main ||
                         (notesAndBoxes && (k == StoryKind::Footnote || k == StoryKind::Endnote || k == StoryKind::TextBox));
    if (!counted) continue;
    for (uint32_t p : doc.StoryParagraphs(s)) total += CountParagraph(doc, p);
  }
  return total;
}

Counts CountRange(const Document& doc, Pos from, Pos to) {
  Counts total;
  if (from.para == kNone || to.para == kNone) return total;
  if (to.para < from.para || (to.para == from.para && to.offset < from.offset)) std::swap(from, to);
  const uint32_t story = doc.Para(from.para).story;
  for (uint32_t p = from.para; p <= to.para && p < doc.Paragraphs().size(); ++p) {
    if (doc.Para(p).story != story) continue;
    total += CountParagraph(doc, p, p == from.para ? from.offset : 0, p == to.para ? to.offset : UINT32_MAX);
  }
  return total;
}

}  // namespace fw::doc::proof
