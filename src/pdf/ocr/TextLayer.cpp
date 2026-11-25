#include "pdf/ocr/TextLayer.h"

#include <fpdf_edit.h>
#include <fpdf_text.h>

#include <algorithm>
#include <cmath>

namespace pdfocr {

namespace {

// Pictures on the page, and in its forms (XObjects) a level or two down.
bool HasPicture(FPDF_PAGEOBJECT obj, int depth) {
  const int type = FPDFPageObj_GetType(obj);
  if (type == FPDF_PAGEOBJ_IMAGE) return true;
  if (type != FPDF_PAGEOBJ_FORM || depth > 2) return false;
  const int n = FPDFFormObj_CountObjects(obj);
  for (int i = 0; i < n; ++i)
    if (HasPicture(FPDFFormObj_GetObject(obj, static_cast<unsigned long>(i)), depth + 1)) return true;
  return false;
}

struct Pt {
  double x = 0, y = 0;
};

Pt ToPage(FPDF_PAGE page, const PageImage& img, double x, double y) {
  // FPDF_DeviceToPage takes whole pixels: a device 16 times larger keeps a sixteenth of one.
  constexpr int k = 16;
  Pt p;
  FPDF_DeviceToPage(page, 0, 0, img.width * k, img.height * k, 0, static_cast<int>(std::lround(x * k)),
                    static_cast<int>(std::lround(y * k)), &p.x, &p.y);
  return p;
}

}  // namespace

bool NeedsText(FPDF_PAGE page) {
  if (!page) return false;
  if (FPDF_TEXTPAGE text = FPDFText_LoadPage(page)) {
    int letters = 0;
    const int n = FPDFText_CountChars(text);
    for (int i = 0; i < n && letters < 8; ++i)
      if (!iswspace(static_cast<wint_t>(FPDFText_GetUnicode(text, i)))) ++letters;
    FPDFText_ClosePage(text);
    if (letters >= 8) return false;
  }
  const int n = FPDFPage_CountObjects(page);
  for (int i = 0; i < n; ++i)
    if (HasPicture(FPDFPage_GetObject(page, i), 0)) return true;
  return false;
}

bool Render(FPDF_PAGE page, int dpi, uint32_t maxSide, PageImage& out) {
  const double w = FPDF_GetPageWidthF(page), h = FPDF_GetPageHeightF(page);
  if (w <= 0 || h <= 0) return false;
  double scale = dpi / 72.0;
  if (maxSide && std::max(w, h) * scale > maxSide) scale = maxSide / std::max(w, h);
  out.width = std::max(1, static_cast<int>(std::floor(w * scale)));
  out.height = std::max(1, static_cast<int>(std::floor(h * scale)));
  out.stride = out.width * 4;
  out.pixels.assign(static_cast<size_t>(out.stride) * out.height, 0xFF);
  FPDF_BITMAP bmp = FPDFBitmap_CreateEx(out.width, out.height, FPDFBitmap_BGRx, out.pixels.data(), out.stride);
  if (!bmp) return false;
  FPDFBitmap_FillRect(bmp, 0, 0, out.width, out.height, 0xFFFFFFFF);
  // As printed: no form highlights, nothing that's only for the screen.
  FPDF_RenderPageBitmap(bmp, page, 0, 0, out.width, out.height, 0, FPDF_ANNOT | FPDF_PRINTING);
  FPDFBitmap_Destroy(bmp);
  return true;
}

int WriteTextLayer(FPDF_DOCUMENT doc, FPDF_PAGE page, const PageImage& img, const Result& result) {
  if (!doc || !page || img.width <= 0) return 0;
  FPDF_FONT font = FPDFText_LoadStandardFont(doc, "Helvetica");
  if (!font) return 0;
  int written = 0;
  for (const Line& line : result.lines) {
    for (const Word& word : line.words) {
      // The box's corners on the page: whatever the page's rotation, the text runs along the
      // bottom edge (left to right) and stands on it.
      const Pt bl = ToPage(page, img, word.x, word.y + word.h), br = ToPage(page, img, word.x + word.w, word.y + word.h),
               tl = ToPage(page, img, word.x, word.y);
      FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, 1.0f);
      if (!obj) continue;
      std::u16string text(word.text.begin(), word.text.end());
      float l = 0, b = 0, r = 0, t = 0;
      if (!FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(text.c_str())) ||
          !FPDFPageObj_GetBounds(obj, &l, &b, &r, &t) || r - l <= 0.01f) {
        FPDFPageObj_Destroy(obj);
        continue;
      }
      FPDFTextObj_SetTextRenderMode(obj, FPDF_TEXTRENDERMODE_INVISIBLE);
      // One unit of text space: the word's width along the bottom edge, its height (an em) up the
      // side; the baseline a little above the bottom, where descenders end.
      const double width = r - l;
      FS_MATRIX m;
      m.a = static_cast<float>((br.x - bl.x) / width);
      m.b = static_cast<float>((br.y - bl.y) / width);
      m.c = static_cast<float>(tl.x - bl.x);
      m.d = static_cast<float>(tl.y - bl.y);
      m.e = static_cast<float>(bl.x + 0.18 * (tl.x - bl.x) - l * m.a);
      m.f = static_cast<float>(bl.y + 0.18 * (tl.y - bl.y) - l * m.b);
      FPDFPageObj_SetMatrix(obj, &m);
      FPDFPage_InsertObject(page, obj);
      ++written;
    }
  }
  FPDFFont_Close(font);
  if (written > 0 && !FPDFPage_GenerateContent(page)) return 0;
  return written;
}

Status RecognizePage(FPDF_DOCUMENT doc, FPDF_PAGE page, int& words, bool force, int dpi) {
  words = 0;
  if (!doc || !page) return Status::Failed;
  if (!force && !NeedsText(page)) return Status::Ok;
  PageImage image;
  if (!Render(page, dpi, MaxDimension(), image)) return Status::Failed;
  Result result;
  const Status s = Recognize(image.pixels.data(), image.width, image.height, image.stride, result);
  if (s == Status::Ok) words = WriteTextLayer(doc, page, image, result);
  return s;
}

}  // namespace pdfocr
