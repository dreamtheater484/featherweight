// The workbook tab's printing: print preview through PagedView, the print dialog, Export PDF, and
// pages as pictures for the pixel compare.

#include <windows.h>

#include <objbase.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "Util.h"
#include "core/Strings.h"
#include "draw/D2DPainter.h"
#include "draw/Pdf.h"
#include "draw/PrintJob.h"
#include "sheet/view/WorkbookView.h"
#include "ui/SwapChain.h"
#include "ui/Theme.h"

using Microsoft::WRL::ComPtr;
using namespace gfx;

namespace fw::sheet::view {

PrintInfo WorkbookView::InfoFor(const Sheet& sheet) const {
  PrintInfo info;
  info.fileName = Title();  // "Book1" before it is saved, as Excel prints it
  info.path = DirOf(path_) + L"\\";
  info.sheetName = Utf8ToWide(sheet.Name());
  wchar_t buf[128];
  if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, buf, 128, nullptr)) info.date = buf;
  if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, buf, 128)) info.time = buf;
  // A sheet that names no paper prints on Letter, as Excel's exports on the reference PC do.
  return info;
}

void WorkbookView::Paginate() {
  prints_.clear();
  pageSizes_.clear();
  auto add = [&](size_t i, PrintInfo info) {
    PrintSheet ps;
    Sheet& s = book_->GetSheet(i);
    ps.objects = SheetObjects::Load(*book_, i);
    info.objects = ps.objects.get();
    info.rules = RulesFor(i);
    if (i != sheet_) {
      ps.grid = std::make_unique<Grid>();
      ps.grid->Build(s, book_->GetStyles(), *measure_);
    }
    ps.layout = std::make_unique<PrintLayout>(*book_, s, i, ps.grid ? *ps.grid : grid_, *text_, info);
    ps.first = pageSizes_.size();
    for (size_t k = 0; k < ps.layout->PageCount(); ++k)
      pageSizes_.push_back({ps.layout->Page(k).width, ps.layout->Page(k).height});
    prints_.push_back(std::move(ps));
  };
  if (printWhat_ != 1) {
    PrintInfo info = InfoFor(Current());
    if (printWhat_ == 2) info.area = {Selected()};
    add(sheet_, info);
    return;
  }
  // The entire workbook, as one job: the first sheet's paper for all, and &N the pages of all (so
  // laid out twice, first to count them).
  uint32_t paper = 0;
  int total = 0;
  for (int pass = 0; pass < 2; ++pass) {
    prints_.clear();
    pageSizes_.clear();
    for (size_t i = 0; i < book_->SheetCount(); ++i) {
      if (!SheetShown(i)) continue;
      Sheet& s = book_->GetSheet(i);
      if (!s.Loaded()) book_->LoadSheet(i);
      if (!paper) paper = s.Page().paperSize ? s.Page().paperSize : 1;
      PrintInfo info = InfoFor(s);
      info.jobPaper = paper;
      info.jobPages = total;
      add(i, info);
    }
    total = static_cast<int>(pageSizes_.size());
  }
}

PrintLayout* WorkbookView::LayoutAt(int page, size_t& local) const {
  for (size_t i = prints_.size(); i-- > 0;)
    if (page >= 0 && static_cast<size_t>(page) >= prints_[i].first) {
      local = static_cast<size_t>(page) - prints_[i].first;
      return local < prints_[i].layout->PageCount() ? prints_[i].layout.get() : nullptr;
    }
  return nullptr;
}

void WorkbookView::ForEachSheetLayout(const std::function<void(PrintLayout&)>& fn) {
  // One job: the first sheet's paper for all, and &N the pages of all (so paginate twice).
  uint32_t paper = 0;
  int pages = 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < book_->SheetCount(); ++i) {
      if (!SheetShown(i)) continue;
      Sheet& s = book_->GetSheet(i);
      if (!s.Loaded()) book_->LoadSheet(i);
      if (!paper) paper = s.Page().paperSize ? s.Page().paperSize : 1;
      PrintInfo info = InfoFor(s);
      info.jobPaper = paper;
      info.jobPages = pages;
      // Objects of its own, placed on the printed columns.
      std::unique_ptr<SheetObjects> objects = SheetObjects::Load(*book_, i);
      info.objects = objects.get();
      info.rules = RulesFor(i);
      Grid other;
      const bool shown = i == sheet_;
      if (!shown) other.Build(s, book_->GetStyles(), *measure_);
      PrintLayout layout(*book_, s, i, shown ? grid_ : other, *text_, info);
      if (pass == 0) pages += static_cast<int>(layout.PageCount());
      else fn(layout);
    }
  }
  text_->Clear();
}

void WorkbookView::TogglePreview() {
  if (!Ready()) return;
  if (!preview_) {
    Paginate();
    if (pageSizes_.empty()) {
      host_.ViewMessage(BannerKind::Info, L"This sheet has nothing to print.");
      return;
    }
    preview_ = true;
    currentPage_ = 0;
    zoomMode_ = fw::ZoomMode::FitPage;
    viewMode_ = fw::ViewMode::Continuous;
    RecomputeScale();
    Relayout(nullptr);
    GoToPage(0);
  } else {
    preview_ = false;
    LayoutParts();
  }
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

void WorkbookView::DrawPreview(Canvas& c, ID2D1DeviceContext* dc) {
  const Palette& p = theme::P();
  c.Fill(RectF(vp_), p.docBg);
  if (prints_.empty()) return;
  StepScroll();
  int first, last;
  VisiblePages(first, last);
  for (int i = first; i <= last && i < static_cast<int>(pageSizes_.size()); ++i) {
    const RECT r = PageRect(i);
    const D2D1_RECT_F rf = RectF(r);
    c.SoftShadow(rf, 1.0f * ui_, 5.0f * ui_, Alpha(p.pageShadow, 0.35f), 1.0f * ui_);
    c.Fill(rf, Rgb(0xFFFFFF));
    const D2D1_RECT_F vis = {std::max(rf.left, static_cast<float>(vp_.left)), std::max(rf.top, static_cast<float>(vp_.top)),
                             std::min(rf.right, static_cast<float>(vp_.right)), std::min(rf.bottom, static_cast<float>(vp_.bottom))};
    if (vis.right <= vis.left || vis.bottom <= vis.top) continue;
    c.PushClip(vis);
    {
      draw::D2DPainter painter(dc);
      const float s = static_cast<float>((r.right - r.left) / std::max(1.0, pageSizes_[i].w));
      painter.Transform(draw::Matrix::Scale(s, s) * draw::Matrix::Translate(static_cast<float>(r.left), static_cast<float>(r.top)));
      size_t local = 0;
      if (PrintLayout* layout = LayoutAt(i, local)) layout->Paint(painter, local);
    }
    c.PopClip();
  }
  PagedView::DrawScrollbars(c);
}

PrintOutcome WorkbookView::Print(HWND owner, const std::function<void(int, int)>& progress, std::wstring& message) {
  if (!Ready()) return PrintOutcome::Failed;
  if (!preview_ || prints_.empty()) Paginate();
  const int pageCount = static_cast<int>(pageSizes_.size());
  if (pageCount == 0) {
    message = L"This sheet has nothing to print.";
    return PrintOutcome::Failed;
  }
  PRINTPAGERANGE ranges[16] = {};
  PRINTDLGEXW pd{sizeof(pd)};
  pd.hwndOwner = owner;
  pd.Flags = PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE | (preview_ ? 0 : PD_NOCURRENTPAGE);
  pd.nMaxPageRanges = 16;
  pd.lpPageRanges = ranges;
  pd.nMinPage = 1;
  pd.nMaxPage = static_cast<DWORD>(pageCount);
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
  std::vector<int> pages;
  if (pd.Flags & PD_PAGENUMS) {
    for (DWORD i = 0; i < pd.nPageRanges; ++i)
      for (DWORD p = ranges[i].nFromPage; p <= ranges[i].nToPage && p >= 1 && p <= static_cast<DWORD>(pageCount); ++p)
        pages.push_back(static_cast<int>(p) - 1);
  } else if (pd.Flags & PD_CURRENTPAGE) {
    pages.push_back(std::min(currentPage_, pageCount - 1));
  } else {
    for (int i = 0; i < pageCount; ++i) pages.push_back(i);
  }
  std::wstring printer;
  if (auto* names = static_cast<DEVNAMES*>(GlobalLock(pd.hDevNames))) {
    printer = reinterpret_cast<const wchar_t*>(names) + names->wDeviceOffset;
    GlobalUnlock(pd.hDevNames);
  }
  const DEVMODEW* devmode = pd.hDevMode ? static_cast<const DEVMODEW*>(GlobalLock(pd.hDevMode)) : nullptr;
  PrintOutcome outcome = PrintPages(printer, devmode, pages, nullptr, progress, message);
  if (devmode) GlobalUnlock(pd.hDevMode);
  cleanup();
  return outcome;
}

PrintOutcome WorkbookView::PrintToFile(const std::wstring& path, std::wstring& message) {
  // Every sheet, as Excel exports a workbook (the reference renders).
  if (!Ready()) return PrintOutcome::Failed;
  ComPtr<IStream> stream;
  if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL,
                                    TRUE, nullptr, &stream))) {
    message = L"Couldn't create " + path;
    return PrintOutcome::Failed;
  }
  draw::PrintJob job;
  HRESULT hr = job.Start(gpu::D2DDevice(), L"Microsoft Print to PDF", nullptr, Title(), stream.Get());
  ForEachSheetLayout([&](PrintLayout& layout) {
    for (size_t i = 0; SUCCEEDED(hr) && i < layout.PageCount(); ++i)
      hr = job.AddPage(layout.Page(i).width, layout.Page(i).height, [&](draw::Painter& p) { layout.Paint(p, i); });
  });
  if (SUCCEEDED(hr)) hr = job.Finish();
  if (FAILED(hr) || job.Wait(120000) != draw::PrintJob::Outcome::Completed) {
    message = L"Printing failed.";
    return PrintOutcome::Failed;
  }
  return PrintOutcome::Printed;
}

PrintOutcome WorkbookView::PrintPages(const std::wstring& printer, const DEVMODEW* devmode, const std::vector<int>& pages,
                                      IStream* output, const std::function<void(int, int)>& progress,
                                      std::wstring& message) {
  if (pages.empty()) {
    message = L"This sheet has nothing to print.";
    return PrintOutcome::Failed;
  }
  draw::PrintJob job;
  HRESULT hr = job.Start(gpu::D2DDevice(), printer, devmode, Title(), output);
  int done = 0;
  for (size_t i = 0; SUCCEEDED(hr) && i < pages.size(); ++i) {
    if (progress) progress(done, static_cast<int>(pages.size()));
    const fw::PageSizePt& page = pageSizes_[static_cast<size_t>(pages[i])];
    hr = job.AddPage(static_cast<float>(page.w), static_cast<float>(page.h),
                     [&](draw::Painter& painter) { PaintPage(painter, pages[i]); });
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

void WorkbookView::PaintPage(draw::Painter& p, int page) {
  size_t local = 0;
  if (PrintLayout* layout = LayoutAt(page, local)) layout->Paint(p, local);
}

bool WorkbookView::ExportPdf(const std::wstring& path, std::wstring& message) {
  if (!Ready()) return false;
  if (prints_.empty() || !preview_) Paginate();
  const size_t count = pageSizes_.size();
  if (count == 0) {
    message = L"This sheet has nothing to export.";
    return false;
  }
  draw::PdfDocument pdf;
  for (size_t i = 0; i < count; ++i)
    pdf.AddPage(static_cast<float>(pageSizes_[i].w), static_cast<float>(pageSizes_[i].h),
                [&](draw::Painter& painter) { PaintPage(painter, static_cast<int>(i)); });
  std::wstring title = Title();
  if (const size_t dot = title.rfind(L'.'); dot != std::wstring::npos && dot > 0) title.resize(dot);
  pdf.SetInfo(std::u16string(title.begin(), title.end()), u"");
  const std::vector<uint8_t> bytes = pdf.Finish();
  if (!util::WriteFileAtomic(path, std::string(bytes.begin(), bytes.end()))) {
    message = L"The PDF couldn't be saved.";
    return false;
  }
  message = count == 1 ? L"Exported 1 page to " + FileNameOf(path)
                       : L"Exported " + std::to_wstring(count) + L" pages to " + FileNameOf(path);
  return true;
}

bool WorkbookView::ExportWorkbookPdfForTesting(const std::wstring& path) {
  // Every sheet's pages in one PDF, as Excel exports a workbook, so a comparison can rasterize
  // ours and Excel's with the same renderer.
  if (!Ready()) return false;
  draw::PdfDocument pdf;
  ForEachSheetLayout([&](PrintLayout& layout) {
    for (size_t i = 0; i < layout.PageCount(); ++i) {
      const PrintPage& page = layout.Page(i);
      pdf.AddPage(page.width, page.height, [&](draw::Painter& painter) { layout.Paint(painter, i); });
    }
  });
  const std::vector<uint8_t> bytes = pdf.Finish();
  return util::WriteFileAtomic(path, std::string(bytes.begin(), bytes.end()));
}

int WorkbookView::RenderPagesForTesting(const std::wstring& dir, float dpi) {
  // Every sheet's pages, numbered on through the workbook, as Excel exports a workbook.
  if (!Ready()) return 0;
  CreateDirectoryW(dir.c_str(), nullptr);
  int written = 0;
  bool failed = false;
  ForEachSheetLayout([&](PrintLayout& layout) {
    if (!failed && !RenderLayoutPages(layout, dir, dpi, written)) failed = true;
  });
  return written;
}

bool WorkbookView::RenderLayoutPages(PrintLayout& layout, const std::wstring& dir, float dpi, int& written) {
  const float scale = dpi / 72;
  for (size_t i = 0; i < layout.PageCount(); ++i) {
    const PrintPage& page = layout.Page(i);
    const UINT w = std::max(1u, static_cast<UINT>(std::ceil(page.width * scale)));
    const UINT h = std::max(1u, static_cast<UINT>(std::ceil(page.height * scale)));
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1DeviceContext> dc;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    if (FAILED(gfx::WIC()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(gfx::D2D()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target)) || FAILED(target.As(&dc)))
      return false;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(1, 1, 1, 1));
    {
      draw::D2DPainter painter(dc.Get());
      painter.Transform(draw::Matrix::Scale(scale, scale));
      layout.Paint(painter, i);
    }
    if (FAILED(dc->EndDraw())) return false;
    const std::wstring file = dir + L"\\page-" + std::to_wstring(written + 1) + L".png";
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(gfx::WIC()->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) ||
        FAILED(gfx::WIC()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
      return false;
    ++written;
  }
  return true;
}

}  // namespace fw::sheet::view
