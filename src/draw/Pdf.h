#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "draw/Painter.h"

// Export PDF (plan §4.6): Featherweight's own small PDF writer, behind Painter. Pages are drawn by
// the same Painter code as the screen and printers, so the PDF looks like what's on screen:
//  - text in the fonts DirectWrite laid it out with, embedded as subsets (TrueType) or whole (CFF),
//    so it stays selectable and searchable. Fonts whose license forbids embedding, variable fonts
//    and CID-keyed CFF fonts are drawn as outlines with invisible text on top;
//  - paths, fills, gradients and clips as vectors, and group opacity as a real transparency group;
//  - JPEG pictures as they are, other pictures compressed with Flate; metafiles and SVG at 300 dpi;
//  - links (web addresses and places in the document), an outline, and the title and author.
namespace draw {

class PdfDocument {
 public:
  PdfDocument();
  ~PdfDocument();
  PdfDocument(const PdfDocument&) = delete;
  PdfDocument& operator=(const PdfDocument&) = delete;

  // A page of `width` x `height` points. `paint` draws it in points with y down, as on screen.
  void AddPage(float width, float height, const std::function<void(Painter&)>& paint);
  size_t PageCount() const;
  // A link over `rect` on `page` (page points): to a web address, or to `toY` on page `toPage`.
  void AddLink(size_t page, const Rect& rect, const std::string& uri);
  void AddLink(size_t page, const Rect& rect, size_t toPage, float toY);
  // The outline (bookmarks), in document order: `level` 0 is the top.
  void AddOutline(int level, const std::u16string& title, size_t page, float y);
  void SetInfo(const std::u16string& title, const std::u16string& author);
  // The file; nothing can be added after it.
  std::vector<uint8_t> Finish();

 private:
  friend class PdfPainter;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace draw
