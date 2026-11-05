#include "doc/edit/Case.h"

#include <windows.h>

#include <algorithm>
#include <vector>

#include "doc/edit/Format.h"

namespace fw::doc::edit {
namespace {

bool IsUpper(char16_t c) { return IsCharUpperW(static_cast<wchar_t>(c)) != FALSE; }
bool IsLower(char16_t c) { return IsCharLowerW(static_cast<wchar_t>(c)) != FALSE; }
bool IsLetter(char16_t c) { return IsCharAlphaW(static_cast<wchar_t>(c)) != FALSE; }

std::u16string Mapped(std::u16string_view text, bool upper) {
  std::u16string s(text);
  if (!s.empty()) {
    auto* p = reinterpret_cast<wchar_t*>(s.data());
    if (upper) CharUpperBuffW(p, static_cast<DWORD>(s.size()));
    else CharLowerBuffW(p, static_cast<DWORD>(s.size()));
  }
  return s;
}

char16_t Upper(char16_t c) { return Mapped(std::u16string_view(&c, 1), true)[0]; }

bool Apostrophe(char16_t c) { return c == u'\'' || c == u'’'; }

// Characters a run's text holds that are just text: not a tab, break, object or field mark.
bool Plain(char16_t c) { return c >= 0x20 && c != 0xFFFC && !(c >= 0xE000 && c <= 0xF8FF); }

// Whether `offset` in a paragraph's text starts a sentence: the paragraph's start, or after the end
// of one (". ", "! ", "? ") with only spaces and quotes between.
bool StartsSentence(std::u16string_view text, uint32_t offset) {
  while (offset > 0) {
    const char16_t c = text[offset - 1];
    if (c == u' ' || c == u' ' || c == u'"' || c == u'“' || c == u'(') {
      --offset;
      continue;
    }
    return c == u'.' || c == u'!' || c == u'?';
  }
  return true;
}

}  // namespace

TextCase NextCase(std::u16string_view text) {
  bool upper = false, lower = false;
  for (char16_t c : text) {
    upper = upper || IsUpper(c);
    lower = lower || IsLower(c);
  }
  if (upper && !lower) return TextCase::Lower;
  if (lower && !upper) return TextCase::Capitalize;
  return TextCase::Upper;
}

std::u16string InCase(std::u16string_view text, TextCase c, bool sentenceStart) {
  switch (c) {
    case TextCase::Upper: return Mapped(text, true);
    case TextCase::Lower: return Mapped(text, false);
    case TextCase::Toggle: {
      std::u16string s(text);
      for (char16_t& ch : s) {
        if (IsUpper(ch)) ch = Mapped(std::u16string_view(&ch, 1), false)[0];
        else if (IsLower(ch)) ch = Upper(ch);
      }
      return s;
    }
    default: break;
  }
  std::u16string s = Mapped(text, false);
  bool start = c == TextCase::Sentence ? sentenceStart : true;
  for (size_t i = 0; i < s.size(); ++i) {
    const char16_t ch = s[i];
    if (IsLetter(ch)) {
      if (start) s[i] = Upper(ch);
      start = false;
    } else if (c == TextCase::Capitalize) {
      // A word goes on through an apostrophe between letters ("don't").
      const bool inWord = Apostrophe(ch) && i > 0 && IsLetter(s[i - 1]) && i + 1 < s.size() && IsLetter(s[i + 1]);
      if (!inWord && !(ch >= u'0' && ch <= u'9')) start = true;
    } else if (ch == u'.' || ch == u'!' || ch == u'?') {
      start = true;
    }
  }
  return s;
}

Result ChangeCase(Editor& ed, Pos from, Pos to, TextCase c) {
  const Document& doc = ed.Doc();
  Result done;
  done.caret = to;
  if (to.para < from.para || (to.para == from.para && to.offset <= from.offset)) return done;
  struct Piece {
    uint32_t para, start;
    std::u16string text;
  };
  std::vector<Piece> pieces;
  static constexpr uint32_t kDel = Name("del"), kMoveFrom = Name("moveFrom");
  for (uint32_t p : ParagraphsOf(doc, {from, to})) {
    const std::u16string& text = doc.Para(p).text;
    const uint32_t a = p == from.para ? std::min<uint32_t>(from.offset, static_cast<uint32_t>(text.size())) : 0;
    const uint32_t b = p == to.para ? std::min<uint32_t>(to.offset, static_cast<uint32_t>(text.size()))
                                    : static_cast<uint32_t>(text.size());
    if (a >= b) continue;
    // The whole range's text in the new case, so a sentence or word that goes on keeps going.
    const std::u16string changed = InCase(std::u16string_view(text).substr(a, b - a), c, StartsSentence(text, a));
    for (const TextRun& r : doc.Runs(p)) {
      if (!r.run || r.fieldCode || r.fieldDepth > 0 || r.simpleField || r.displayOnly) continue;
      if (r.revision && (r.revision.Is(kDel) || r.revision.Is(kMoveFrom))) continue;
      // In this run, each stretch of plain text from its first changed letter to its last.
      uint32_t i = std::max(a, r.start);
      const uint32_t end = std::min(b, r.end);
      while (i < end) {
        while (i < end && !Plain(text[i])) ++i;
        uint32_t j = i;
        while (j < end && Plain(text[j])) ++j;
        uint32_t first = j, last = i;
        for (uint32_t k = i; k < j; ++k)
          if (changed[k - a] != text[k]) {
            first = std::min(first, k);
            last = k + 1;
          }
        if (first < last) pieces.push_back({p, first, changed.substr(first - a, last - first)});
        i = j;
      }
    }
  }
  if (pieces.empty()) return done;
  // From the end, so the positions before stay. Each stretch goes in after itself, taking its own
  // run's formatting (the text before it), then the old letters go.
  std::sort(pieces.begin(), pieces.end(),
            [](const Piece& x, const Piece& y) { return x.para != y.para ? x.para > y.para : x.start > y.start; });
  ed.BeginGroup(u"Change case");
  for (const Piece& piece : pieces) {
    const uint32_t end = piece.start + static_cast<uint32_t>(piece.text.size());
    Result r = ed.InsertText({piece.para, end}, piece.text);
    if (r) r = ed.Delete({piece.para, piece.start}, {piece.para, end});
    if (!r) {
      ed.CancelGroup();
      return r;
    }
  }
  ed.EndGroup();
  return done;
}

}  // namespace fw::doc::edit
