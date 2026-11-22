// The presentation tab's printing and Export PDF (slides, handouts, notes pages), its file state,
// Save As (a copy, as opened) and the scripted runs' steps.

#include <windows.h>

#include <objbase.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <winspool.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Util.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "draw/Pdf.h"
#include "draw/PrintJob.h"
#include "slide/view/ObjectTool.h"
#include "slide/view/PresentationView.h"
#include "slide/view/SlidePages.h"
#include "text/Fonts.h"
#include "ui/SwapChain.h"

using Microsoft::WRL::ComPtr;

namespace fw::slide::view {

namespace {

// The paper a printer prints on with these settings, points; Letter when it can't tell.
draw::Size PaperOf(const std::wstring& printer, const DEVMODEW* devmode) {
  draw::Size paper{612, 792};
  if (HDC ic = CreateICW(nullptr, printer.c_str(), nullptr, devmode)) {
    const int dx = GetDeviceCaps(ic, LOGPIXELSX), dy = GetDeviceCaps(ic, LOGPIXELSY);
    const int w = GetDeviceCaps(ic, PHYSICALWIDTH), h = GetDeviceCaps(ic, PHYSICALHEIGHT);
    if (dx > 0 && dy > 0 && w > 0 && h > 0) paper = {w * 72.0f / dx, h * 72.0f / dy};
    DeleteDC(ic);
  }
  return paper;
}

// The printer's settings with the orientation PowerPoint picks for the layout: slides as wide as
// the deck's (landscape for a landscape deck), handouts and notes pages portrait.
std::vector<uint8_t> Oriented(const std::wstring& printer, const DEVMODEW* devmode, bool landscape) {
  std::vector<uint8_t> copy;
  if (devmode) {
    copy.resize(devmode->dmSize + devmode->dmDriverExtra);
    std::memcpy(copy.data(), devmode, copy.size());
  } else {
    // No settings from a dialog: the printer's defaults.
    HANDLE h = nullptr;
    if (!OpenPrinterW(const_cast<wchar_t*>(printer.c_str()), &h, nullptr)) return copy;
    const LONG size = DocumentPropertiesW(nullptr, h, const_cast<wchar_t*>(printer.c_str()), nullptr, nullptr, 0);
    if (size > 0) {
      copy.resize(static_cast<size_t>(size));
      if (DocumentPropertiesW(nullptr, h, const_cast<wchar_t*>(printer.c_str()), reinterpret_cast<DEVMODEW*>(copy.data()),
                              nullptr, DM_OUT_BUFFER) != IDOK)
        copy.clear();
    }
    ClosePrinter(h);
    if (copy.empty()) return copy;
  }
  auto* dm = reinterpret_cast<DEVMODEW*>(copy.data());
  dm->dmFields |= DM_ORIENTATION;
  dm->dmOrientation = landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
  return copy;
}

}  // namespace

text::Fonts& PresentationView::PageFonts() {
  if (!fonts_) fonts_ = std::make_unique<text::Fonts>();
  return *fonts_;
}

std::vector<size_t> PresentationView::PrintedSlides() const {
  std::vector<size_t> out;
  for (size_t i = 0; i < SlideCount(); ++i)
    if (print_.includeHidden || !deck_->Hidden(i)) out.push_back(i);
  return out;
}

std::vector<PrintPage> PresentationView::Pages(draw::Size slidePaper, draw::Size paper) const {
  return LayoutPages(PrintedSlides(), deck_->SlideSize(), slidePaper, paper, print_);
}

// ---------------------------------------------------------------------------
// Print

PrintOutcome PresentationView::Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) {
  if (!CanPrint()) return PrintOutcome::Failed;
  // The dialog's pages are the slides, as in PowerPoint: "2-5" prints those slides in the layout chosen.
  const int count = static_cast<int>(SlideCount());
  PRINTPAGERANGE ranges[16] = {};
  PRINTDLGEXW pd{sizeof(pd)};
  pd.hwndOwner = owner;
  pd.Flags = PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
  pd.nMaxPageRanges = 16;
  pd.lpPageRanges = ranges;
  pd.nMinPage = 1;
  pd.nMaxPage = static_cast<DWORD>(count);
  pd.nCopies = 1;
  pd.nStartPage = START_PAGE_GENERAL;
  HRESULT hr = PrintDlgExW(&pd);
  auto cleanup = [&]() {
    if (pd.hDC) DeleteDC(pd.hDC);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
  };
  if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDevNames) {
    cleanup();
    if (FAILED(hr)) {
      message = L"The print dialog couldn’t be opened. Is a printer installed?";
      return PrintOutcome::Failed;
    }
    return PrintOutcome::Cancelled;
  }
  std::vector<int> slides;
  if (pd.Flags & PD_PAGENUMS) {
    for (DWORD i = 0; i < pd.nPageRanges; ++i)
      for (DWORD p = ranges[i].nFromPage; p <= ranges[i].nToPage && p >= 1 && p <= static_cast<DWORD>(count); ++p)
        slides.push_back(static_cast<int>(p) - 1);
  } else if (pd.Flags & PD_CURRENTPAGE) {
    slides.push_back(static_cast<int>(current_));
  } else {
    slides.push_back(-1);  // every slide (hidden ones as chosen)
  }
  std::wstring printer;
  if (auto* names = static_cast<DEVNAMES*>(GlobalLock(pd.hDevNames))) {
    printer = reinterpret_cast<const wchar_t*>(names) + names->wDeviceOffset;
    GlobalUnlock(pd.hDevNames);
  }
  const DEVMODEW* devmode = pd.hDevMode ? static_cast<const DEVMODEW*>(GlobalLock(pd.hDevMode)) : nullptr;
  PrintOutcome outcome = PrintTo(printer, devmode, slides, nullptr, progress, message);
  if (devmode) GlobalUnlock(pd.hDevMode);
  cleanup();
  return outcome;
}

PrintOutcome PresentationView::PrintToFile(const std::wstring& path, std::wstring& message) {
  if (!CanPrint()) return PrintOutcome::Failed;
  ComPtr<IStream> stream;
  if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL,
                                    TRUE, nullptr, &stream))) {
    message = L"Couldn't create " + path;
    return PrintOutcome::Failed;
  }
  return PrintTo(L"Microsoft Print to PDF", nullptr, {-1}, stream.Get(), {}, message);
}

PrintOutcome PresentationView::PrintTo(const std::wstring& printer, const DEVMODEW* devmode, const std::vector<int>& chosen,
                                       IStream* output, const std::function<void(int, int)>& progress,
                                       std::wstring& message) {
  std::vector<size_t> slides;
  if (chosen.size() == 1 && chosen[0] < 0) {
    slides = PrintedSlides();
  } else {
    // Slides named in the range print even when hidden, as PowerPoint prints them.
    for (int s : chosen)
      if (s >= 0 && static_cast<size_t>(s) < SlideCount()) slides.push_back(static_cast<size_t>(s));
  }
  if (slides.empty()) {
    message = L"There are no slides to print. Hidden slides print when Print hidden slides is on.";
    return PrintOutcome::Failed;
  }
  const draw::Size slide = deck_->SlideSize();
  const bool landscape = print_.what == PrintWhat::Slides && slide.width > slide.height;
  const std::vector<uint8_t> oriented = Oriented(printer, devmode, landscape);
  const DEVMODEW* dm = oriented.empty() ? devmode : reinterpret_cast<const DEVMODEW*>(oriented.data());
  draw::Size paper = PaperOf(printer, dm);
  // A driver that ignored the orientation: turn the page ourselves.
  if (landscape != (paper.width > paper.height) && (print_.what == PrintWhat::Slides || paper.width > paper.height))
    std::swap(paper.width, paper.height);
  const std::vector<PrintPage> pages = LayoutPages(slides, slide, paper, paper, print_);
  draw::PrintJob job;
  HRESULT hr = job.Start(gpu::D2DDevice(), printer, dm, Title(), output);
  int done = 0;
  for (size_t i = 0; SUCCEEDED(hr) && i < pages.size(); ++i) {
    if (progress) progress(done, static_cast<int>(pages.size()));
    hr = job.AddPage(pages[i].size.width, pages[i].size.height,
                     [&](draw::Painter& painter) { PaintPage(*deck_, pages[i], print_, PageFonts(), painter); });
    if (SUCCEEDED(hr)) ++done;
  }
  if (SUCCEEDED(hr)) hr = job.Finish();
  if (FAILED(hr)) {
    message = L"Printing stopped because the printer reported an error.";
    return PrintOutcome::Failed;
  }
  switch (job.Wait(120000)) {
    case draw::PrintJob::Outcome::Canceled: message = L"Printing was canceled."; return PrintOutcome::Cancelled;
    case draw::PrintJob::Outcome::Failed:
      message = L"Printing stopped because the printer reported an error.";
      return PrintOutcome::Failed;
    default: break;
  }
  message = done == 1 ? L"Sent 1 page to the printer" : L"Sent " + std::to_wstring(done) + L" pages to the printer";
  return PrintOutcome::Printed;
}

// ---------------------------------------------------------------------------
// Export PDF: slides at the slide's size, handouts and notes pages at the notes page's, as
// PowerPoint exports them; the slides' web links stay clickable and links between slides jump.

bool PresentationView::ExportPdf(const std::wstring& path, std::wstring& message) {
  if (!CanExportPdf()) return false;
  const std::vector<PrintPage> pages = Pages(deck_->SlideSize(), deck_->NotesSize());
  if (pages.empty()) {
    message = L"There are no slides to export. Hidden slides export when Print hidden slides is on.";
    return false;
  }
  draw::PdfDocument pdf;
  std::vector<int> pageOf(SlideCount(), -1);  // the page each slide is on, for links between slides
  for (size_t i = 0; i < pages.size(); ++i)
    for (const PageItem& it : pages[i].items)
      if (it.slide < pageOf.size() && pageOf[it.slide] < 0) pageOf[it.slide] = static_cast<int>(i);
  const draw::Size slide = deck_->SlideSize();
  for (size_t i = 0; i < pages.size(); ++i) {
    pdf.AddPage(pages[i].size.width, pages[i].size.height,
                [&](draw::Painter& painter) { PaintPage(*deck_, pages[i], print_, PageFonts(), painter); });
    for (const PageItem& it : pages[i].items) {
      const float k = it.rect.Width() / std::max(1.0f, slide.width);
      for (const SlideLink& link : deck_->Links(it.slide)) {
        const draw::Rect r{it.rect.left + link.rect.left * k, it.rect.top + link.rect.top * k,
                           it.rect.left + link.rect.right * k, it.rect.top + link.rect.bottom * k};
        if (!link.url.empty() && util::IsSafeExternalLink(link.url)) {
          pdf.AddLink(i, r, WideToUtf8(link.url));
        } else if (link.slide >= 0 && static_cast<size_t>(link.slide) < pageOf.size() && pageOf[link.slide] >= 0) {
          pdf.AddLink(i, r, static_cast<size_t>(pageOf[link.slide]), 0);
        }
      }
    }
  }
  std::wstring title = deck_->Title();
  if (title.empty()) {
    title = Title();
    if (const size_t dot = title.rfind(L'.'); dot != std::wstring::npos && dot > 0) title.resize(dot);
  }
  pdf.SetInfo(std::u16string(title.begin(), title.end()), u"");
  const std::vector<uint8_t> bytes = pdf.Finish();
  if (!util::WriteFileAtomic(path, std::string(bytes.begin(), bytes.end()))) {
    message = L"The PDF couldn't be saved.";
    return false;
  }
  message = pages.size() == 1 ? L"Exported 1 page to " + FileNameOf(path)
                              : L"Exported " + std::to_wstring(pages.size()) + L" pages to " + FileNameOf(path);
  return true;
}

int PresentationView::RenderPagesForTesting(const std::wstring& dir, float dpi) {
  if (!Ready()) return 0;
  CreateDirectoryW(dir.c_str(), nullptr);
  // Full page slides (the default): every slide, hidden ones too, in order, at the slide's size, for
  // the pixel compare (p1-corpus's slide-pages.ps1). Handouts and notes pages as they export.
  std::vector<size_t> slides = PrintedSlides();
  if (print_.what == PrintWhat::Slides) {
    slides.resize(SlideCount());
    for (size_t i = 0; i < slides.size(); ++i) slides[i] = i;
  }
  const std::vector<PrintPage> pages = LayoutPages(slides, deck_->SlideSize(), deck_->SlideSize(), deck_->NotesSize(), print_);
  const float scale = dpi / 72;
  int written = 0;
  for (const PrintPage& page : pages) {
    const UINT w = std::max(1u, static_cast<UINT>(std::ceil(page.size.width * scale)));
    const UINT h = std::max(1u, static_cast<UINT>(std::ceil(page.size.height * scale)));
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1DeviceContext> dc;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    if (FAILED(gfx::WIC()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(gfx::D2D()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc)))
      return written;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(1, 1, 1, 1));
    {
      draw::D2DPainter painter(dc.Get());
      painter.Transform(draw::Matrix::Scale(scale, scale));
      PaintPage(*deck_, page, print_, PageFonts(), painter);
    }
    if (FAILED(dc->EndDraw())) return written;
    const std::wstring file = dir + L"\\page-" + std::to_wstring(written + 1) + L".png";
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(gfx::WIC()->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) ||
        FAILED(gfx::WIC()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
      return written;
    ++written;
  }
  return written;
}

// ---------------------------------------------------------------------------
// File

SaveFormat PresentationView::SaveAsFormat() const {
  static const SaveFormat kinds[] = {
      {L"PowerPoint presentation", L"*.pptx", L"pptx"},
      {L"PowerPoint macro-enabled presentation", L"*.pptm", L"pptm"},
      {L"PowerPoint template", L"*.potx", L"potx"},
      {L"PowerPoint macro-enabled template", L"*.potm", L"potm"},
      {L"PowerPoint slide show", L"*.ppsx", L"ppsx"},
  };
  for (const SaveFormat& f : kinds)
    if (EndsWithNoCase(path_, std::wstring(L".") + f.ext)) return f;
  return kinds[0];
}

bool PresentationView::Save(const std::wstring& path, std::wstring& error) {
  // Read-only until P2: Save As writes the file exactly as it was opened, and only as its own type.
  if (!Ready()) return false;
  if (EqualsNoCase(util::FullPath(path), util::FullPath(path_))) return true;
  if (!EndsWithNoCase(path, std::wstring(L".") + SaveAsFormat().ext)) {
    error = L"Presentations can only be saved as a copy of the file for now. Export PDF works for other formats.";
    return false;
  }
  std::string bytes;
  if (!util::ReadFileBytes(path_, bytes, size_t{1} << 30) || !util::WriteFileAtomic(path, bytes)) {
    error = L"The copy couldn't be saved.";
    return false;
  }
  return true;
}

FileState PresentationView::State() const {
  FileState s;
  s.page = static_cast<int>(current_);
  s.zoomMode = zoom_ > 0 ? fw::ZoomMode::Custom : fw::ZoomMode::FitPage;
  s.zoom = zoom_ > 0 ? zoom_ : 1;
  return s;
}

void PresentationView::ApplyState(const FileState& s) {
  if (!Ready()) return;
  zoom_ = s.zoomMode == fw::ZoomMode::Custom ? std::clamp(s.zoom, 0.1, 5.0) : 0;
  current_ = 0;
  if (s.page > 0) GoToSlide(static_cast<size_t>(s.page));
  LayoutParts();
}

bool PresentationView::ScriptOp(const std::wstring& op, const std::wstring& arg) {
  if (Ready() && Objects().ScriptOp(op, arg)) return true;  // p2-objects: insert:, shapefill:
  // slide:N (1-based) goes to a slide; printlayout:slides|notes|handouts6 chooses what prints and
  // exports; printhidden:1 prints hidden slides too; notes:0 hides the notes pane.
  if (op == L"slide") {
    int n = 0;
    if (ParseInt(arg, n) && n >= 1) GoToSlide(static_cast<size_t>(n - 1));
    return true;
  }
  if (op == L"printlayout") {
    if (arg == L"slides") print_.what = PrintWhat::Slides;
    else if (arg == L"notes") print_.what = PrintWhat::Notes;
    else if (arg.rfind(L"handouts", 0) == 0) {
      int n = 6;
      ParseInt(arg.substr(8), n);
      print_.what = PrintWhat::Handouts;
      print_.perPage = ValidHandoutCount(n) ? n : 6;
    }
    return true;
  }
  if (op == L"printhidden") {
    print_.includeHidden = arg != L"0";
    return true;
  }
  if (op == L"notes") {
    notesShown_ = arg != L"0";
    LayoutParts();
    slideBitmap_.Reset();
    return true;
  }
  return false;
}

}  // namespace fw::slide::view
