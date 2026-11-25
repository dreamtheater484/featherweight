#pragma once

// Windows' own offline OCR (Windows.Media.Ocr) for scanned pages (F1, f1-pdf). It reads the
// languages Windows already has; nothing is downloaded and nothing leaves the PC. The engine's
// DLL loads only when text is first recognized. Each call blocks: run it off the UI thread.

#include <cstdint>
#include <string>
#include <vector>

namespace pdfocr {

// A recognized word and its box, in the bitmap's pixels.
struct Word {
  std::wstring text;
  float x = 0, y = 0, w = 0, h = 0;
};
struct Line {
  std::vector<Word> words;
};
struct Result {
  std::vector<Line> lines;
  std::wstring language;  // the BCP-47 tag the engine read with ("en-US")
  double angle = 0;       // the text's slant in degrees, when the engine found one
};

enum class Status { Ok, NoEngine, NoLanguage, Failed };

// The longest side a bitmap may have (OcrEngine.MaxImageDimension), or 0 without the engine.
uint32_t MaxDimension();
// Recognizes the text in a top-down 32-bit BGRA bitmap with the first of the user's languages
// that OCR can read. Initializes the Windows Runtime on the calling thread if it isn't yet.
Status Recognize(const uint8_t* bgra, int width, int height, int stride, Result& out);

}  // namespace pdfocr
