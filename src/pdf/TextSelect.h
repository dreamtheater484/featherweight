#pragma once

#include <fpdfview.h>

#include <string>
#include <vector>

class Document;

struct TextPos {
  int page = -1;
  int index = -1;
  bool Valid() const { return page >= 0 && index >= 0; }
  bool operator<(const TextPos& o) const { return page != o.page ? page < o.page : index < o.index; }
  bool operator==(const TextPos& o) const { return page == o.page && index == o.index; }
};

// A text selection that may span pages. Both ends are inclusive character positions.
struct TextSelection {
  TextPos anchor, focus;
  int version = 0;  // bumps on every change so cached rectangles can be refreshed

  bool Empty() const { return !anchor.Valid() || !focus.Valid(); }
  void Clear() {
    anchor = focus = TextPos{};
    ++version;
  }
  void Set(TextPos a, TextPos f) {
    anchor = a;
    focus = f;
    ++version;
  }
  TextPos Start() const { return focus < anchor ? focus : anchor; }
  TextPos End() const { return focus < anchor ? anchor : focus; }
  bool Covers(int page) const { return !Empty() && page >= Start().page && page <= End().page; }
};

namespace textsel {

std::wstring Text(Document& doc, const TextSelection& sel, int maxChars = 4 << 20);
// Rectangles (page coordinates) of the selected text on `page`.
std::vector<FS_RECTF> Rects(Document& doc, const TextSelection& sel, int page);
// Expands `index` to the surrounding word.
void WordAt(Document& doc, int page, int index, int& start, int& end);
int CharCount(Document& doc, int page);

}  // namespace textsel
