// The PDF tab's tools (F1, f1-pdf): page operations through PDFium, checked on documents made
// here, saved and read back as a reader would.
#include "TestHarness.h"

#include <windows.h>

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pdf/ocr/Ocr.h"
#include "pdf/ocr/TextLayer.h"
#include "pdf/tools/PageOps.h"

namespace {

void InitPdfium() {
  static bool done = false;
  if (done) return;
#ifdef FW_PDFIUM_DLL
  // pdfium.dll is delay-loaded: the build's own copy, loaded first, is the one bound.
  std::string dll = FW_PDFIUM_DLL;
  for (char& ch : dll)
    if (ch == '/') ch = '\\';
  LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
#endif
  FPDF_LIBRARY_CONFIG config{};
  config.version = 2;
  FPDF_InitLibraryWithConfig(&config);
  done = true;
}

// A document whose page i is (100 + i) points wide, so each page can be told apart after a move.
FPDF_DOCUMENT MakeDoc(int pages, int widthBase = 100) {
  InitPdfium();
  FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
  for (int i = 0; i < pages; ++i) {
    FPDF_PAGE page = FPDFPage_New(doc, i, widthBase + i, 500);
    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
  }
  return doc;
}

// The width of each page, as the page order shows it.
std::vector<int> Widths(FPDF_DOCUMENT doc) {
  std::vector<int> out;
  for (int i = 0; i < FPDF_GetPageCount(doc); ++i) {
    FS_SIZEF s{};
    FPDF_GetPageSizeByIndexF(doc, i, &s);
    // A quarter turn swaps the sides: the original width is the shorter one here.
    out.push_back(static_cast<int>(std::lround(std::min(s.width, s.height))));
  }
  return out;
}

int RotationOf(FPDF_DOCUMENT doc, int index) {
  FPDF_PAGE page = FPDF_LoadPage(doc, index);
  const int r = FPDFPage_GetRotation(page);
  FPDF_ClosePage(page);
  return r;
}

// Saved and opened again: what another reader gets.
struct Reopened {
  std::string bytes;
  FPDF_DOCUMENT doc = nullptr;
  explicit Reopened(FPDF_DOCUMENT from, bool incremental = false) {
    if (pdftools::SaveBytes(from, incremental, bytes)) doc = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
  }
  ~Reopened() {
    if (doc) FPDF_CloseDocument(doc);
  }
};

}  // namespace

TEST(F1PdfRotateSavesAndReadsBack) {
  FPDF_DOCUMENT doc = MakeDoc(3);
  CHECK(pdftools::Rotate(doc, {0, 2}, 1));
  CHECK(pdftools::Rotate(doc, {2}, 1));
  CHECK(pdftools::Rotate(doc, {1}, -1));
  Reopened back(doc);
  CHECK(back.doc != nullptr);
  CHECK_EQ(RotationOf(back.doc, 0), 1);
  CHECK_EQ(RotationOf(back.doc, 1), 3);
  CHECK_EQ(RotationOf(back.doc, 2), 2);
  CHECK(!pdftools::Rotate(doc, {7}, 1));  // no such page
  FPDF_CloseDocument(doc);
}

TEST(F1PdfDeleteKeepsAtLeastOnePage) {
  FPDF_DOCUMENT doc = MakeDoc(5);
  CHECK(pdftools::Delete(doc, {3, 1, 3}));
  CHECK((Widths(doc) == std::vector<int>{100, 102, 104}));
  CHECK(!pdftools::Delete(doc, {0, 1, 2}));  // every page: refused, nothing removed
  CHECK_EQ(FPDF_GetPageCount(doc), 3);
  Reopened back(doc, true);
  CHECK((Widths(back.doc) == std::vector<int>{100, 102, 104}));
  FPDF_CloseDocument(doc);
}

TEST(F1PdfMoveToEveryGap) {
  CHECK_EQ(pdftools::MoveDestination({3}, 1), 1);
  CHECK_EQ(pdftools::MoveDestination({0, 1}, 4), 2);
  CHECK_EQ(pdftools::MoveDestination({1, 3}, 3), 2);
  // [A B C D E]: B and D before A; then A at the end; then a move onto itself.
  FPDF_DOCUMENT doc = MakeDoc(5);
  CHECK(pdftools::Move(doc, {3, 1}, 0));
  CHECK((Widths(doc) == std::vector<int>{101, 103, 100, 102, 104}));
  CHECK(pdftools::Move(doc, {2}, 5));
  CHECK((Widths(doc) == std::vector<int>{101, 103, 102, 104, 100}));
  CHECK(pdftools::Move(doc, {1, 2}, 2));
  CHECK((Widths(doc) == std::vector<int>{101, 103, 102, 104, 100}));
  CHECK(!pdftools::Move(doc, {1}, 9));
  Reopened back(doc, true);
  CHECK((Widths(back.doc) == std::vector<int>{101, 103, 102, 104, 100}));
  FPDF_CloseDocument(doc);
}

TEST(F1PdfInsertMergesAnotherFile) {
  FPDF_DOCUMENT doc = MakeDoc(2);
  FPDF_DOCUMENT other = MakeDoc(3, 200);
  std::string bytes;
  CHECK(pdftools::SaveBytes(other, false, bytes));
  FPDF_CloseDocument(other);
  pdftools::Source src;
  CHECK(pdftools::OpenSource(bytes, {}, src) == pdftools::SourceError::None);
  CHECK_EQ(pdftools::Insert(doc, src, 1), 3);
  CHECK((Widths(doc) == std::vector<int>{100, 200, 201, 202, 101}));
  CHECK_EQ(pdftools::Insert(doc, src, 99), 3);  // past the end: at the end
  CHECK_EQ(FPDF_GetPageCount(doc), 8);
  Reopened back(doc, true);
  CHECK_EQ(FPDF_GetPageCount(back.doc), 8);
  CHECK_EQ(Widths(back.doc).back(), 202);
  FPDF_CloseDocument(doc);

  pdftools::Source junk;
  CHECK(pdftools::OpenSource("not a PDF at all", {}, junk) == pdftools::SourceError::NotPdf);
  pdftools::Source none;
  CHECK(pdftools::OpenSource({}, {}, none) == pdftools::SourceError::Read);
}

TEST(F1PdfExtractAndSplit) {
  FPDF_DOCUMENT doc = MakeDoc(7);
  std::string part;
  CHECK(pdftools::Extract(doc, {4, 1}, part));
  FPDF_DOCUMENT p = FPDF_LoadMemDocument64(part.data(), part.size(), nullptr);
  CHECK(p != nullptr);
  CHECK((Widths(p) == std::vector<int>{104, 101}));
  FPDF_CloseDocument(p);
  CHECK(!pdftools::Extract(doc, {9}, part));

  auto every = pdftools::SplitEvery(7, 3);
  CHECK_EQ(every.size(), size_t{3});
  CHECK((every[2] == std::vector<int>{6}));
  auto at = pdftools::SplitAt(7, {5, 2, 2});
  CHECK_EQ(at.size(), size_t{3});
  CHECK((at[0] == std::vector<int>{0, 1}));
  CHECK((at[1] == std::vector<int>{2, 3, 4}));
  CHECK((at[2] == std::vector<int>{5, 6}));
  CHECK_EQ(pdftools::SplitAt(3, {}).size(), size_t{1});
  CHECK(pdftools::PageList({0, 1, 2, 4, 6, 7}) == L"1-3, 5, 7-8");
  CHECK(pdftools::PageList({3}) == L"4");
  CHECK(pdftools::PageToolsBlock(doc, false) == pdftools::Block::None);
  CHECK(pdftools::PageToolsBlock(doc, true) == pdftools::Block::Xfa);
  FPDF_CloseDocument(doc);
}

// ---- OCR (Windows.Media.Ocr) as an invisible text layer

namespace {

// A "scan": a page whose only content is a picture of text, as a scanner makes it.
FPDF_DOCUMENT MakeScan(const wchar_t* line1, const wchar_t* line2) {
  InitPdfium();
  // The text, typeset by PDFium and rendered at 200 dpi.
  FPDF_DOCUMENT src = FPDF_CreateNewDocument();
  FPDF_PAGE sp = FPDFPage_New(src, 0, 612, 792);
  FPDF_FONT font = FPDFText_LoadStandardFont(src, "Times-Roman");
  int y = 700;
  for (const wchar_t* text : {line1, line2}) {
    FPDF_PAGEOBJECT t = FPDFPageObj_CreateTextObj(src, font, 28.0f);
    std::u16string u(text, text + wcslen(text));
    FPDFText_SetText(t, reinterpret_cast<FPDF_WIDESTRING>(u.c_str()));
    FPDFPageObj_Transform(t, 1, 0, 0, 1, 72, y);
    FPDFPage_InsertObject(sp, t);
    y -= 48;
  }
  FPDFPage_GenerateContent(sp);
  const int w = 612 * 200 / 72, h = 792 * 200 / 72;
  FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 0);
  FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(bmp, sp, 0, 0, w, h, 0, 0);
  FPDF_ClosePage(sp);
  FPDFFont_Close(font);
  // The picture alone on a page of a new document.
  FPDF_DOCUMENT doc = FPDF_CreateNewDocument();
  FPDF_PAGE page = FPDFPage_New(doc, 0, 612, 792);
  FPDF_PAGEOBJECT image = FPDFPageObj_NewImageObj(doc);
  FPDFImageObj_SetBitmap(nullptr, 0, image, bmp);
  FPDFImageObj_SetMatrix(image, 612, 0, 0, 792, 0, 0);
  FPDFPage_InsertObject(page, image);
  FPDFPage_GenerateContent(page);
  FPDF_ClosePage(page);
  FPDFBitmap_Destroy(bmp);
  FPDF_CloseDocument(src);
  return doc;
}

std::wstring PageText(FPDF_DOCUMENT doc, int index) {
  FPDF_PAGE page = FPDF_LoadPage(doc, index);
  FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
  const int n = FPDFText_CountChars(tp);
  std::wstring out(static_cast<size_t>(n) + 1, L'\0');
  FPDFText_GetText(tp, 0, n, reinterpret_cast<unsigned short*>(out.data()));
  out.resize(static_cast<size_t>(n));
  FPDFText_ClosePage(tp);
  FPDF_ClosePage(page);
  return out;
}

}  // namespace

TEST(F1PdfOcrWritesFindableText) {
  FPDF_DOCUMENT doc = MakeScan(L"Featherweight reads scanned pages", L"The quick brown fox jumps over the lazy dog");
  FPDF_PAGE page = FPDF_LoadPage(doc, 0);
  CHECK(pdfocr::NeedsText(page));
  pdfocr::PageImage img;
  CHECK(pdfocr::Render(page, 300, pdfocr::MaxDimension(), img));
  pdfocr::Result result;
  const pdfocr::Status status = pdfocr::Recognize(img.pixels.data(), img.width, img.height, img.stride, result);
  if (status == pdfocr::Status::NoEngine || status == pdfocr::Status::NoLanguage) {
    std::printf("  (no OCR language on this PC: skipped)\n");
    FPDF_ClosePage(page);
    FPDF_CloseDocument(doc);
    return;
  }
  CHECK(status == pdfocr::Status::Ok);
  CHECK(pdfocr::WriteTextLayer(doc, page, img, result) >= 12);
  FPDF_ClosePage(page);
  Reopened back(doc, true);
  CHECK(back.doc != nullptr);
  const std::wstring text = PageText(back.doc, 0);
  for (const wchar_t* word : {L"Featherweight", L"scanned", L"quick", L"brown", L"lazy"})
    CHECK(text.find(word) != std::wstring::npos);
  // The page looks as it did: the text is invisible, the picture is still the page.
  FPDF_PAGE again = FPDF_LoadPage(back.doc, 0);
  CHECK(!pdfocr::NeedsText(again));
  FPDF_ClosePage(again);
  FPDF_CloseDocument(doc);
}
