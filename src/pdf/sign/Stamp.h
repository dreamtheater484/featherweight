#pragma once

// A drawn signature placed as a stamp (F1, f1-pdf): what was drawn with a pen or the mouse, or a
// picture of a signature, as a stamp annotation on a page, through PDFium (fpdf_annot). Other
// readers show and print it as it is; it can be removed again like a note.

#include <fpdf_annot.h>
#include <fpdfview.h>

#include <cstdint>
#include <vector>

namespace pdfsign {

struct Ink {
  // Strokes drawn, each a run of points from 0 to 1 across the drawing's width and down its
  // height (y grows downwards, as it was drawn).
  std::vector<std::vector<std::pair<float, float>>> strokes;
  float aspect = 3;  // the drawing's width over its height
  // Or a picture: top-down BGRA; white and near-white become transparent.
  std::vector<uint8_t> bgra;
  int width = 0, height = 0;

  bool Empty() const { return strokes.empty() && bgra.empty(); }
  float Aspect() const { return bgra.empty() ? aspect : static_cast<float>(width) / static_cast<float>(height); }
};

// The strokes in a box of their own: their bounds become 0..1, the aspect is theirs.
Ink InkFromPoints(const std::vector<std::vector<std::pair<float, float>>>& points);

// Puts the signature on `page` in `rect` (page coordinates). Returns the annotation's index, or -1.
int AddSignatureStamp(FPDF_DOCUMENT doc, FPDF_PAGE page, const FS_RECTF& rect, const Ink& ink);

// Where a signature `widthPt` wide goes when it's centred on (x, y), kept inside the page.
FS_RECTF StampRect(FPDF_PAGE page, float x, float y, float widthPt, float aspect);

}  // namespace pdfsign
