// The PDF tab's tools (F1, f1-pdf): page operations through PDFium, checked on documents made
// here, saved and read back as a reader would.
#include "TestHarness.h"

#include <windows.h>

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_signature.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

#include <roapi.h>
#include <windows.applicationmodel.datatransfer.h>
#include <windows.storage.h>
#include <winstring.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "Share.h"
#include "pdf/ocr/Ocr.h"
#include "pdf/sign/PdfObjects.h"
#include "pdf/sign/Sign.h"
#include "pdf/ocr/TextLayer.h"
#include "pdf/tools/Editor.h"
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
  // With the spaces between them, as a phrase search and copying need (f1-gate's finding 8).
  CHECK(text.find(L"quick brown fox") != std::wstring::npos);
  CHECK(text.find(L"reads scanned pages") != std::wstring::npos);
  // The page looks as it did: the text is invisible, the picture is still the page.
  FPDF_PAGE again = FPDF_LoadPage(back.doc, 0);
  CHECK(!pdfocr::NeedsText(again));
  FPDF_ClosePage(again);
  FPDF_CloseDocument(doc);
}

// ---- Certificate signatures (PAdES B-B through CNG) and the object reader under them

namespace {

std::string ReadAll(const std::string& path) {
  std::string out;
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
  }
  return out;
}

const CERT_CONTEXT* TestSigner() {
  std::string dir = FW_F1PDF_TESTDATA;
  return pdfsign::LoadPfx(ReadAll(dir + "/signing/test-signer.pfx"), L"featherweight");
}

struct SigInfo {
  int count = 0;
  pdfsign::Verdict verdict;
};

// What a reader finds: PDFium's signature objects, checked against the bytes.
SigInfo Check(const std::string& bytes, int index = 0) {
  SigInfo info;
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
  if (!doc) return info;
  info.count = FPDF_GetSignatureCount(doc);
  if (FPDF_SIGNATURE sig = FPDF_GetSignatureObject(doc, index)) {
    std::vector<uint8_t> contents(FPDFSignatureObj_GetContents(sig, nullptr, 0));
    FPDFSignatureObj_GetContents(sig, contents.data(), static_cast<unsigned long>(contents.size()));
    std::vector<int> range(FPDFSignatureObj_GetByteRange(sig, nullptr, 0));
    FPDFSignatureObj_GetByteRange(sig, range.data(), static_cast<unsigned long>(range.size()));
    info.verdict = pdfsign::Verify(bytes, range, contents);
  }
  FPDF_CloseDocument(doc);
  return info;
}

// A PDF with its objects in an object stream and a cross-reference stream, as PDF 1.5 writers make
// them (not compressed, so it can be written here).
std::string XrefStreamPdf() {
  const std::string o1 = "<</Type /Catalog /Pages 2 0 R>>", o2 = "<</Type /Pages /Kids [3 0 R] /Count 1>>",
                    o3 = "<</Type /Page /Parent 2 0 R /MediaBox [0 0 300 300]>>";
  const std::string head = "1 0 2 " + std::to_string(o1.size() + 1) + " 3 " + std::to_string(o1.size() + o2.size() + 2) + " ";
  const std::string body = o1 + " " + o2 + " " + o3;
  std::string pdf = "%PDF-1.5\n%\xE2\xE3\xCF\xD3\n";
  const size_t objStm = pdf.size();
  pdf += "4 0 obj\n<</Type /ObjStm /N 3 /First " + std::to_string(head.size()) + " /Length " +
         std::to_string(head.size() + body.size()) + ">>\nstream\n" + head + body + "\nendstream\nendobj\n";
  const size_t xref = pdf.size();
  std::string rows;
  auto row = [&](int type, size_t a, int b) {
    rows += static_cast<char>(type);
    rows += static_cast<char>((a >> 8) & 0xFF);
    rows += static_cast<char>(a & 0xFF);
    rows += static_cast<char>(b);
  };
  row(0, 0, 0);
  row(2, 4, 0);
  row(2, 4, 1);
  row(2, 4, 2);
  row(1, objStm, 0);
  row(1, xref, 0);
  pdf += "5 0 obj\n<</Type /XRef /Size 6 /W [1 2 1] /Root 1 0 R /Length " + std::to_string(rows.size()) +
         ">>\nstream\n" + rows + "\nendstream\nendobj\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

}  // namespace

TEST(F1PdfObjectReaderFollowsXrefStreams) {
  InitPdfium();
  const std::string pdf = XrefStreamPdf();
  pdfobj::File f;
  CHECK(f.Open(pdf));
  CHECK(f.LastXrefIsStream());
  CHECK_EQ(f.Size(), 6);
  CHECK_EQ(f.FirstPage(), 3);
  pdfobj::Obj page;
  CHECK(f.Get(3, page));
  CHECK(page.Get("MediaBox") && page.Get("MediaBox")->items.size() == 4);
  // Written back as it was read.
  pdfobj::Obj catalog;
  CHECK(f.Get(1, catalog));
  CHECK_EQ(pdfobj::Write(catalog), std::string("<</Type /Catalog/Pages 2 0 R>>"));
  size_t at = 0;
  pdfobj::Obj o;
  const std::string sample = R"x(<</A [1 2.5 (a\)b) <0aF> /N#20x true null 7 0 R]>>)x";
  CHECK(pdfobj::ParseObject(sample, at, o));
  CHECK_EQ(pdfobj::Write(o), sample);
  CHECK(!f.Open("%PDF-1.4 nothing else in here at all, no xref"));
  // PDFium reads it too.
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(pdf.data(), pdf.size(), nullptr);
  CHECK(doc != nullptr);
  CHECK_EQ(FPDF_GetPageCount(doc), 1);
  FPDF_CloseDocument(doc);
}

TEST(F1PdfSignsAndCatchesChanges) {
  InitPdfium();
  const CERT_CONTEXT* cert = TestSigner();
  CHECK(cert != nullptr);
  if (!cert) return;
  FPDF_DOCUMENT doc = MakeDoc(2);
  std::string bytes;
  CHECK(pdftools::SaveBytes(doc, false, bytes));
  FPDF_CloseDocument(doc);

  pdfsign::Details d;
  d.name = L"Featherweight Test Signer";
  std::string signedPdf;
  CHECK(pdfsign::SignPdf(bytes, cert, d, signedPdf) == pdfsign::Error::None);
  CHECK(signedPdf.compare(0, bytes.size(), bytes) == 0);  // an update after the file, which stays as it was
  SigInfo info = Check(signedPdf);
  CHECK_EQ(info.count, 1);
  // Intact; the test certificate is its own issuer, so Windows doesn't trust it.
  CHECK(info.verdict.kind == pdfsign::Verdict::Untrusted);
  CHECK(info.verdict.signer == L"Featherweight Test Signer");

  // A byte changed inside what was signed: invalid.
  std::string tampered = signedPdf;
  const size_t mediaBox = tampered.find("/MediaBox");
  CHECK(mediaBox != std::string::npos);
  tampered[mediaBox + 12] = tampered[mediaBox + 12] == '1' ? '2' : '1';
  CHECK(Check(tampered).verdict.kind == pdfsign::Verdict::Invalid);

  // A change saved after signing, as Featherweight saves a signed PDF (an incremental update):
  // changed after signing, the signed revision intact.
  FPDF_DOCUMENT again = FPDF_LoadMemDocument64(signedPdf.data(), signedPdf.size(), nullptr);
  CHECK(pdftools::Rotate(again, {0}, 1));
  std::string changed;
  CHECK(pdftools::SaveDocument(again, signedPdf.size(), changed));
  FPDF_CloseDocument(again);
  CHECK(changed.size() > signedPdf.size() && changed.compare(0, signedPdf.size(), signedPdf) == 0);
  CHECK(Check(changed).verdict.kind == pdfsign::Verdict::Changed);
  // Only what changed is written after the signature: the turned page, not the form, the
  // signature field or the signature (f1-gate's finding 9; PDFium alone wrote them all again).
  const std::string update = changed.substr(signedPdf.size());
  CHECK(update.find("/ByteRange") == std::string::npos && update.find("/SigFlags") == std::string::npos);
  size_t written = 0;
  for (size_t at = update.find(" obj"); at != std::string::npos; at = update.find(" obj", at + 4)) ++written;
  CHECK_EQ(written, size_t{1});

  // Signing again adds a second signature after the first; a file with a cross-reference stream
  // gets one too.
  std::string twice;
  CHECK(pdfsign::SignPdf(signedPdf, cert, d, twice) == pdfsign::Error::None);
  CHECK_EQ(Check(twice).count, 2);
  CHECK(Check(twice, 1).verdict.kind == pdfsign::Verdict::Untrusted);
  CHECK(Check(twice, 0).verdict.kind == pdfsign::Verdict::Changed);
  std::string streamSigned;
  CHECK(pdfsign::SignPdf(XrefStreamPdf(), cert, d, streamSigned) == pdfsign::Error::None);
  info = Check(streamSigned);
  CHECK_EQ(info.count, 1);
  CHECK(info.verdict.kind == pdfsign::Verdict::Untrusted);
  // Changed and saved: PDFium's update after a cross-reference stream gets its /Type /XRef.
  FPDF_DOCUMENT streamDoc = FPDF_LoadMemDocument64(streamSigned.data(), streamSigned.size(), nullptr);
  CHECK(pdftools::Rotate(streamDoc, {0}, 1));
  std::string streamChanged;
  CHECK(pdftools::SaveDocument(streamDoc, streamSigned.size(), streamChanged));
  FPDF_CloseDocument(streamDoc);
  CHECK(streamChanged.compare(0, streamSigned.size(), streamSigned) == 0);
  pdfobj::File typed;
  CHECK(typed.Open(streamChanged));
  CHECK(typed.LastXrefIsStream() && typed.Trailer().Get("Type") && typed.Trailer().Get("Type")->IsName("XRef"));
  CHECK(Check(streamChanged).verdict.kind == pdfsign::Verdict::Changed);
  CertFreeCertificateContext(cert);
}

// The share sheet's part that can run without showing it: a real DataPackage, as the sheet's
// request hands one over, gets the file as its one storage item, read back as a target app reads it.
TEST(F1PdfShareSheetPackagesTheFile) {
  namespace wf = ABI::Windows::Foundation;
  namespace wfc = ABI::Windows::Foundation::Collections;
  namespace dt = ABI::Windows::ApplicationModel::DataTransfer;
  namespace ws = ABI::Windows::Storage;
  using Microsoft::WRL::ComPtr;
  wchar_t full[MAX_PATH] = {};
  const std::string rel = std::string(FW_F1PDF_TESTDATA) + "/text.pdf";
  GetFullPathNameW(std::wstring(rel.begin(), rel.end()).c_str(), MAX_PATH, full, nullptr);
  const std::wstring path = full;
  bool put = false, read = false;
  unsigned count = 0;
  std::wstring got;
  // Windows' data transfer runs in the multithreaded apartment, as the sheet's file lookup does.
  std::thread([&] {
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return;
    {
      ComPtr<IInspectable> made;
      ComPtr<dt::IDataPackage> package;
      const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (SUCCEEDED(RoActivateInstance(Microsoft::WRL::Wrappers::HStringReference(
                                           RuntimeClass_Windows_ApplicationModel_DataTransfer_DataPackage).Get(),
                                       &made)) &&
          SUCCEEDED(made.As(&package))) {
        put = share::PutFileForTesting(package.Get(), path, done) && WaitForSingleObject(done, 10000) == WAIT_OBJECT_0;
        ComPtr<dt::IDataPackageView> view;
        ComPtr<wf::IAsyncOperation<wfc::IVectorView<ws::IStorageItem*>*>> op;
        ComPtr<wf::IAsyncInfo> info;
        if (put && SUCCEEDED(package->GetView(&view)) && SUCCEEDED(view->GetStorageItemsAsync(&op)) &&
            SUCCEEDED(op.As(&info))) {
          wf::AsyncStatus status = wf::AsyncStatus::Started;
          for (int i = 0; i < 1000 && status == wf::AsyncStatus::Started; ++i) {
            Sleep(10);
            info->get_Status(&status);
          }
          ComPtr<wfc::IVectorView<ws::IStorageItem*>> items;
          ComPtr<ws::IStorageItem> item;
          HSTRING name = nullptr;
          if (status == wf::AsyncStatus::Completed && SUCCEEDED(op->GetResults(&items)) &&
              SUCCEEDED(items->get_Size(&count)) && count == 1 && SUCCEEDED(items->GetAt(0, &item)) &&
              SUCCEEDED(item->get_Path(&name))) {
            got = WindowsGetStringRawBuffer(name, nullptr);
            read = true;
          }
          WindowsDeleteString(name);
        }
      }
      CloseHandle(done);
    }
    RoUninitialize();
  }).join();
  CHECK(put);
  CHECK(read);
  CHECK_EQ(count, 1u);
  CHECK(_wcsicmp(got.c_str(), path.c_str()) == 0);
}

// A PDF from its objects (1..n, in order), with a classic cross-reference table.
static std::string ClassicPdf(const std::vector<std::string>& objects) {
  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> at;
  for (size_t k = 0; k < objects.size(); ++k) {
    at.push_back(pdf.size());
    pdf += std::to_string(k + 1) + " 0 obj\n" + objects[k] + "\nendobj\n";
  }
  const size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f\r\n";
  for (size_t a : at) {
    char row[24];
    std::snprintf(row, sizeof row, "%010zu 00000 n\r\n", a);
    pdf += row;
  }
  pdf += "trailer\n<</Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R>>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

// Two branches of the page tree: the first turned and landscape with a font, the second portrait
// without. Pages 0-1 are in the first, 2-3 in the second.
static std::string TwoBranchPdf() {
  const std::string content = "BT /F1 24 Tf 72 72 Td (Hello) Tj ET";
  return ClassicPdf({
      "<</Type /Catalog /Pages 2 0 R>>",
      "<</Type /Pages /Kids [3 0 R 4 0 R] /Count 4>>",
      "<</Type /Pages /Parent 2 0 R /Kids [5 0 R 6 0 R] /Count 2 /Rotate 90 /MediaBox [0 0 842 595] /Resources 9 0 R>>",
      "<</Type /Pages /Parent 2 0 R /Kids [7 0 R 8 0 R] /Count 2 /MediaBox [0 0 595 842] /Resources <<>>>>",
      "<</Type /Page /Parent 3 0 R /Contents 10 0 R>>",
      "<</Type /Page /Parent 3 0 R>>",
      "<</Type /Page /Parent 4 0 R>>",
      "<</Type /Page /Parent 4 0 R>>",
      "<</Font <</F1 <</Type /Font /Subtype /Type1 /BaseFont /Helvetica>>>>>>",
      "<</Length " + std::to_string(content.size()) + ">>\nstream\n" + content + "\nendstream",
  });
}

// f1-gate's finding: FPDF_MovePages gives a moved page its new parent's inherited attributes. Two
// branches: the first turned and landscape with a font, the second portrait without.
TEST(F1PdfMoveKeepsInheritedAttributes) {
  InitPdfium();
  const std::string pdf = TwoBranchPdf();
  auto movedLooksRight = [](FPDF_DOCUMENT doc) {
    // The first page, moved to the end, is still turned and landscape.
    FPDF_PAGE page = FPDF_LoadPage(doc, 3);
    FS_RECTF box{};
    const bool ok = page && FPDFPage_GetRotation(page) == 1 && FPDF_GetPageBoundingBox(page, &box) &&
                    std::fabs(box.right - 842) < 0.5f && std::fabs(box.top - 595) < 0.5f;
    if (page) FPDF_ClosePage(page);
    return ok;
  };

  // PDFium's functions alone: the rotation and the visible area.
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(pdf.data(), pdf.size(), nullptr);
  CHECK(doc != nullptr);
  CHECK(pdftools::Move(doc, {0}, 4));
  CHECK(movedLooksRight(doc));
  FPDF_CloseDocument(doc);

  // Pinned first: everything inherited, the font's resources too, is on the page.
  std::string pinned;
  CHECK(pdftools::PinInherited(pdf, {0}, 4, pinned));
  CHECK(pinned.compare(0, pdf.size(), pdf) == 0);
  pdfobj::File f;
  CHECK(f.Open(pinned));
  pdfobj::Obj page;
  CHECK(f.Get(5, page));
  CHECK(page.Get("Resources") && page.Get("Resources")->type == pdfobj::Obj::Ref && page.Get("Resources")->num == 9);
  CHECK(page.Get("Rotate") && page.Get("Rotate")->Int() == 90);
  CHECK(page.Get("MediaBox") && page.Get("MediaBox")->items.size() == 4);
  pdfobj::Obj untouched;
  CHECK(f.Get(6, untouched) && !untouched.Get("Rotate"));  // only the pages that move
  doc = FPDF_LoadMemDocument64(pinned.data(), pinned.size(), nullptr);
  CHECK(doc != nullptr);
  CHECK(pdftools::Move(doc, {0}, 4));
  CHECK(movedLooksRight(doc));
  // Saved whole and read again, the same.
  std::string saved;
  CHECK(pdftools::SaveDocument(doc, pinned.size(), saved));
  FPDF_CloseDocument(doc);
  doc = FPDF_LoadMemDocument64(saved.data(), saved.size(), nullptr);
  CHECK(doc != nullptr);
  CHECK(movedLooksRight(doc));
  FPDF_CloseDocument(doc);

  // One branch: a move keeps every page's parent, nothing to pin.
  FPDF_DOCUMENT flat = MakeDoc(3);
  std::string flatBytes, none;
  CHECK(pdftools::SaveBytes(flat, false, flatBytes));
  FPDF_CloseDocument(flat);
  CHECK(!pdftools::PinInherited(flatBytes, {0}, 3, none));
}

// The tools without a window, as fwtool's PDF torture and OCR runs call them (f1-gate).
TEST(F1PdfEditorWithoutAWindow) {
  InitPdfium();
  const std::string original = ReadAll(std::string(FW_F1PDF_TESTDATA) + "/text.pdf");
  pdftools::Editor ed;
  CHECK(ed.Open(original) == pdftools::SourceError::None);
  CHECK(ed.PageToolsBlocked() == pdftools::Block::None);
  // Unedited: the bytes as they were.
  std::string bytes;
  CHECK(ed.Bytes(bytes) && bytes == original);
  const int pages = ed.PageCount();
  CHECK(pages >= 2);
  CHECK(ed.Rotate({0}, 1));
  CHECK(ed.Move({0}, pages));
  pdfsign::Ink ink = pdfsign::InkFromPoints({{{0.f, 0.f}, {10.f, 5.f}, {20.f, 0.f}}});
  CHECK(ed.PlaceSignature(0, 200, 200, 150, ink));
  CHECK(ed.Modified());
  // A certificate signature after the edits: the signed bytes are the document's now.
  const CERT_CONTEXT* cert = TestSigner();
  CHECK(cert != nullptr);
  if (cert) {
    pdfsign::Details d;
    d.name = L"Featherweight test";
    CHECK(ed.Sign(cert, d) == pdfsign::Error::None);
    CertFreeCertificateContext(cert);
  }
  CHECK(!ed.Modified());
  CHECK(ed.Bytes(bytes));
  CHECK(Check(bytes).verdict.kind == pdfsign::Verdict::Untrusted);
  // Saved safely, the same bytes; then a change keeps the signed revision (changed after signing).
  wchar_t dir[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, dir);
  const std::wstring path = std::wstring(dir) + L"fw-f1pdf-editor-" + std::to_wstring(GetCurrentProcessId()) + L".pdf";
  std::wstring error;
  CHECK(ed.SaveTo(path, error));
  std::string written;
  {
    std::string narrow;  // the temporary folder's path is ASCII here
    for (wchar_t ch : path) narrow += static_cast<char>(ch);
    written = ReadAll(narrow);
  }
  CHECK(written == bytes);
  DeleteFileW(path.c_str());
  CHECK(ed.Rotate({0}, 1));
  std::string after;
  CHECK(ed.Bytes(after));
  CHECK(after.compare(0, bytes.size(), bytes) == 0);
  CHECK(Check(after).verdict.kind == pdfsign::Verdict::Changed);

  // An XFA form: no page tools, no signing, and both say so.
  const std::string xfa = ClassicPdf({
      "<</Type /Catalog /Pages 2 0 R /AcroForm <</Fields [] /XFA (<xdp/>)>> /NeedsRendering true>>",
      "<</Type /Pages /Kids [3 0 R] /Count 1>>",
      "<</Type /Page /Parent 2 0 R /MediaBox [0 0 300 300]>>",
  });
  pdftools::Editor form;
  CHECK(form.Open(xfa) == pdftools::SourceError::None);
  CHECK(form.PageToolsBlocked() == pdftools::Block::Xfa);
  CHECK(!form.Rotate({0}, 1));
  CHECK(form.SigningBlocked() == pdfsign::Error::Encrypted);
}

// f1-gate's round 2, finding 5: pages inserted from another PDF read from it until the document is
// written and opened again; the Editor does that before returning, so the source may close.
TEST(F1PdfInsertSurvivesItsSourceClosing) {
  InitPdfium();
  // A source whose first page has an annotation (a drawn signature), whose streams PDFium reads late.
  FPDF_DOCUMENT made = MakeDoc(2);
  FPDF_PAGE page = FPDF_LoadPage(made, 0);
  CHECK(page != nullptr);
  const pdfsign::Ink ink = pdfsign::InkFromPoints({{{0.f, 0.f}, {10.f, 5.f}, {20.f, 0.f}}});
  CHECK(pdfsign::AddSignatureStamp(made, page, pdfsign::StampRect(page, 50, 250, 60, ink.Aspect()), ink) >= 0);
  FPDF_ClosePage(page);
  std::string source;
  CHECK(pdftools::SaveBytes(made, false, source));
  FPDF_CloseDocument(made);

  pdftools::Editor ed;
  CHECK(ed.Open(source) == pdftools::SourceError::None);
  {
    pdftools::Source src;
    CHECK(pdftools::OpenSource(source, {}, src) == pdftools::SourceError::None);
    CHECK_EQ(ed.Insert(src, ed.PageCount()), 2);
  }  // the source closes here
  std::string out;
  CHECK(ed.Bytes(out));
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(out.data(), out.size(), nullptr);
  CHECK(doc != nullptr);
  CHECK_EQ(FPDF_GetPageCount(doc), 4);
  FPDF_PAGE copied = FPDF_LoadPage(doc, 2);
  CHECK(copied && FPDFPage_GetAnnotCount(copied) == 1);
  if (copied) FPDF_ClosePage(copied);
  FPDF_CloseDocument(doc);
}

// Round 2, finding 6: an inserted page without a /Rotate of its own keeps the rotation it had,
// not the one its new branch passes down.
TEST(F1PdfInsertKeepsTheSourcesRotation) {
  InitPdfium();
  const std::string pdf = TwoBranchPdf();
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument64(pdf.data(), pdf.size(), nullptr);
  CHECK(doc != nullptr);
  FPDF_DOCUMENT one = MakeDoc(1);  // 100 x 500, no /Rotate
  std::string oneBytes;
  CHECK(pdftools::SaveBytes(one, false, oneBytes));
  FPDF_CloseDocument(one);
  pdftools::Source src;
  CHECK(pdftools::OpenSource(oneBytes, {}, src) == pdftools::SourceError::None);
  CHECK_EQ(pdftools::Insert(doc, src, 1), 1);  // into the turned branch
  FPDF_PAGE page = FPDF_LoadPage(doc, 1);
  FS_RECTF box{};
  CHECK(page && FPDFPage_GetRotation(page) == 0 && FPDF_GetPageBoundingBox(page, &box));
  CHECK(std::fabs(box.right - box.left - 100) < 0.5f && std::fabs(box.top - box.bottom - 500) < 0.5f);
  if (page) FPDF_ClosePage(page);
  FPDF_CloseDocument(doc);
}

// Round 2, finding 7: signing finds the first page past a branch left empty by deleting its pages.
TEST(F1PdfSignsAfterTheFirstBranchIsEmptied) {
  InitPdfium();
  pdftools::Editor ed;
  CHECK(ed.Open(TwoBranchPdf()) == pdftools::SourceError::None);
  CHECK(ed.Delete({0, 1}));
  std::string bytes;
  CHECK(ed.Bytes(bytes));
  pdfobj::File f;
  CHECK(f.Open(bytes));
  CHECK(f.FirstPage() >= 0);
  const CERT_CONTEXT* cert = TestSigner();
  CHECK(cert != nullptr);
  if (!cert) return;
  pdfsign::Details d;
  d.name = L"Featherweight test";
  CHECK(ed.Sign(cert, d) == pdfsign::Error::None);
  CertFreeCertificateContext(cert);
  CHECK(ed.Bytes(bytes));
  CHECK(Check(bytes).verdict.kind == pdfsign::Verdict::Untrusted);
}

// fuzz_pdfpages' regressions (tests/fuzz/regressions/pdfpages): every object compares as the same as
// itself, keys written twice and numbers past a double's range included.
TEST(F1PdfSameObjectMatchesItself) {
  auto same = [](const std::string& x, const std::string& y) {
    pdfobj::Obj a, b;
    size_t i = 0, j = 0;
    if (!pdfobj::ParseObject(x, i, a) || !pdfobj::ParseObject(y, j, b)) return false;
    pdfobj::File none;
    return pdfobj::SameObject(none, a, none, b);
  };
  CHECK(same("<</A 1/A [2]>>", "<</A 1/A [2]>>"));
  CHECK(!same("<</A 1/A [2]>>", "<</A [2]/A 1>>"));
  CHECK(!same("<</A 1/A [2]>>", "<</A 1>>"));
  CHECK(same("<</A 1/B 2/A 3>>", "<</B 2/A 1/A 3>>"));
  CHECK(same("<</A 1/B (x)>>", "<</B <78>/A 1.00001>>"));
  const std::string huge = "1" + std::string(400, '0');
  CHECK(same("[" + huge + "]", "[" + huge + "]"));
  CHECK(!same("[" + huge + "]", "[2" + huge.substr(1) + "]"));
}
