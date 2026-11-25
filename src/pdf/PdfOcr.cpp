// Recognize text (F1, f1-pdf): Windows' OCR on scanned pages, written as an invisible text layer
// (pdf/ocr). PDFium isn't thread-safe, so the UI thread renders each page and writes its words;
// only Windows' recognizer runs on a thread of its own, which wakes the window when it's done.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include "pdf/PdfText.h"
#include "pdf/PdfView.h"
#include "pdf/ocr/TextLayer.h"

namespace {
constexpr int kOcrDpi = 300;
}

struct PdfView::OcrJob {
  std::vector<int> pages;  // to look at, in order
  size_t next = 0;
  int page = -1;  // the one the recognizer is reading
  int recognized = 0, words = 0;
  uint32_t maxSide = 0;
  HWND wake = nullptr;
  pdfocr::PageImage image;
  pdfocr::Result result;
  pdfocr::Status status = pdfocr::Status::Ok;
  std::atomic<bool> busy{false}, ready{false};
  bool stop = false;
};

bool PdfView::OcrHasWork() const { return ocr_ && (ocr_->ready || !ocr_->busy); }

void PdfView::StartOcr(std::vector<int> pages) {
  if (!Ready()) return;
  if (ocr_) {
    StopOcr();
    return;
  }
  if (pages.empty())
    for (int i = 0; i < PageCount(); ++i) pages.push_back(i);
  if (!BeginPageChange()) return;
  auto job = std::make_shared<OcrJob>();
  job->pages = std::move(pages);
  job->wake = host_.ViewHwnd();
  job->maxSide = pdfocr::MaxDimension();
  ocr_ = job;
  host_.ViewMessage(BannerKind::Info, T(L"Recognizing text… Esc stops."));
  host_.ViewStateChanged();
}

void PdfView::StopOcr() {
  if (!ocr_) return;
  ocr_->stop = true;
  if (!ocr_->busy) FinishOcr();
}

bool PdfView::OcrStep() {
  OcrJob& job = *ocr_;
  if (job.ready) {
    // The recognizer is done with a page: its words go onto it.
    job.ready = false;
    job.busy = false;
    if (job.status == pdfocr::Status::Ok) {
      const int n = pdfocr::WriteTextLayer(doc_->Handle(), doc_->Page(job.page), job.image, job.result);
      if (n > 0) {
        ++job.recognized;
        job.words += n;
        cache_->InvalidatePage(job.page);
      }
    } else if (job.status != pdfocr::Status::Failed) {
      job.stop = true;  // no engine or no language: the same for every page
    }
    job.image = {};
    ++job.next;
  }
  if (job.busy) return false;
  if (job.stop || job.next >= job.pages.size()) {
    FinishOcr();
    return true;
  }
  // The next page that looks scanned (one look per slice: each loads a page).
  const int page = job.pages[job.next];
  FPDF_PAGE p = doc_->Page(page);
  if (!p || !pdfocr::NeedsText(p)) {
    ++job.next;
    return false;
  }
  if (!pdfocr::Render(p, kOcrDpi, job.maxSide, job.image)) {
    ++job.next;
    return false;
  }
  job.page = page;
  job.busy = true;
  host_.ViewMessage(BannerKind::Info, pdftext::Fill(T(L"Recognizing text on page {0} of {1}… Esc stops."),
                                                    {std::to_wstring(page + 1), std::to_wstring(PageCount())}));
  std::shared_ptr<OcrJob> shared = ocr_;
  std::thread([shared] {
    shared->status = pdfocr::Recognize(shared->image.pixels.data(), shared->image.width, shared->image.height,
                                       shared->image.stride, shared->result);
    shared->ready = true;
    PostMessageW(shared->wake, WM_NULL, 0, 0);  // wakes the window's message loop
  }).detach();
  return false;
}

void PdfView::FinishOcr() {
  std::shared_ptr<OcrJob> job = std::move(ocr_);
  ocr_.reset();
  if (!job) return;
  const bool changed = job->recognized > 0;
  if (!changed) pageBefore_.clear();  // nothing to undo
  EndPageChange(changed, CurrentPage());
  std::wstring message;
  BannerKind kind = BannerKind::Success;
  if (job->status == pdfocr::Status::NoEngine || job->status == pdfocr::Status::NoLanguage) {
    kind = BannerKind::Error;
    message = T(L"Windows can't recognize text in any of your languages. A language with its optical character "
                L"recognition can be added in Windows' language settings.");
  } else if (changed) {
    message = pdftext::Fill(T(L"Recognized {0} words on {1}. They can be found, selected and copied now."),
                            {std::to_wstring(job->words), pdftext::Pages(job->recognized)});
  } else if (job->stop) {
    kind = BannerKind::Info;
    message = T(L"Stopped recognizing text.");
  } else {
    kind = BannerKind::Info;
    message = T(L"No scanned pages without text were found.");
  }
  host_.ViewMessage(kind, message);
}

int PdfView::OcrNow(const std::vector<int>& pagesIn) {
  if (!Ready() || ocr_) return 0;
  std::vector<int> pages = pagesIn;
  if (pages.empty())
    for (int i = 0; i < PageCount(); ++i) pages.push_back(i);
  if (!BeginPageChange()) return 0;
  const uint32_t maxSide = pdfocr::MaxDimension();
  int recognized = 0;
  for (int page : pages) {
    FPDF_PAGE p = doc_->Page(page);
    pdfocr::PageImage image;
    pdfocr::Result result;
    if (!p || !pdfocr::NeedsText(p) || !pdfocr::Render(p, kOcrDpi, maxSide, image)) continue;
    if (pdfocr::Recognize(image.pixels.data(), image.width, image.height, image.stride, result) != pdfocr::Status::Ok)
      break;
    if (pdfocr::WriteTextLayer(doc_->Handle(), doc_->Page(page), image, result) > 0) ++recognized;
  }
  if (recognized == 0) pageBefore_.clear();
  EndPageChange(recognized > 0, CurrentPage());
  return recognized;
}
