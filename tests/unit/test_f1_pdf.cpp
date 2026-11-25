// The PDF tab's tools (F1, f1-pdf): page operations through PDFium, checked on documents made
// here, saved and read back as a reader would.
#include "TestHarness.h"

#include <windows.h>

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdfview.h>

#include <cmath>
#include <string>
#include <vector>

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
