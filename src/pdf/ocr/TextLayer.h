#pragma once

// Scanned pages made searchable (F1, f1-pdf): a page is rendered for OCR, and what Windows' OCR
// reads is written onto it as invisible text (text render mode 3) over each word, so the text can
// be found, selected and copied while the page looks exactly as before. Through PDFium's own
// functions (fpdf_edit); the scan itself isn't touched.

#include <fpdfview.h>

#include <cstdint>
#include <vector>

#include "pdf/ocr/Ocr.h"

namespace pdfocr {

// A page that looks scanned: hardly any text, and at least one picture.
bool NeedsText(FPDF_PAGE page);

// The page as OCR reads it: top-down BGRA at up to `dpi`, its longest side at most `maxSide`
// (0: no limit).
struct PageImage {
  std::vector<uint8_t> pixels;
  int width = 0, height = 0, stride = 0;
};
bool Render(FPDF_PAGE page, int dpi, uint32_t maxSide, PageImage& out);

// Writes the words onto the page as invisible text (Helvetica, which every reader has, so nothing
// is embedded), each scaled over its box, then the page's content. Returns the words written.
int WriteTextLayer(FPDF_DOCUMENT doc, FPDF_PAGE page, const PageImage& image, const Result& result);

}  // namespace pdfocr
