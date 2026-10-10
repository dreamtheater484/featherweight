#include "Print.h"

#include <ole2.h>
#include <commdlg.h>
#include <fpdf_flatten.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>
#include <fpdfview.h>
#include <prsht.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Document.h"
#include "resource.h"

namespace print {
namespace {

bool g_actualSize = false;  // remembered for the session

INT_PTR CALLBACK OptionsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
  switch (msg) {
    case WM_INITDIALOG:
      CheckRadioButton(dlg, IDC_PRINT_FIT, IDC_PRINT_ACTUAL, g_actualSize ? IDC_PRINT_ACTUAL : IDC_PRINT_FIT);
      return TRUE;
    case WM_COMMAND:
      if (HIWORD(wp) == BN_CLICKED) {
        if (LOWORD(wp) == IDC_PRINT_FIT) g_actualSize = false;
        if (LOWORD(wp) == IDC_PRINT_ACTUAL) g_actualSize = true;
      }
      break;
  }
  return FALSE;
}

struct MemWriter : FPDF_FILEWRITE {
  std::string data;
};

int WriteToMemory(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
  static_cast<MemWriter*>(self)->data.append(static_cast<const char*>(data), size);
  return 1;
}

// A flattened, unencrypted copy of the document: form field values become regular page
// content, so they print as crisp vectors through FPDF_RenderPage.
class PrintCopy {
 public:
  bool Create(Document& doc) {
    if (doc.Form()) FORM_ForceToKillFocus(doc.Form());
    MemWriter w;
    w.version = 1;
    w.WriteBlock = WriteToMemory;
    if (!FPDF_SaveAsCopy(doc.Handle(), &w, FPDF_REMOVE_SECURITY)) return false;
    data_ = std::move(w.data);
    doc_ = FPDF_LoadMemDocument64(data_.data(), data_.size(), nullptr);
    return doc_ != nullptr;
  }
  ~PrintCopy() {
    if (doc_) FPDF_CloseDocument(doc_);
  }
  // Loads a page with its annotations and form fields flattened into the content.
  FPDF_PAGE FlatPage(int index) {
    FPDF_PAGE page = FPDF_LoadPage(doc_, index);
    if (!page) return nullptr;
    if (FPDFPage_Flatten(page, FLAT_PRINT) == FLATTEN_SUCCESS) {
      FPDF_ClosePage(page);
      page = FPDF_LoadPage(doc_, index);  // re-parse the new content stream
    }
    return page;
  }

 private:
  std::string data_;
  FPDF_DOCUMENT doc_ = nullptr;
};

void RenderToPrinter(HDC dc, FPDF_PAGE page) {
  const int physW = GetDeviceCaps(dc, PHYSICALWIDTH), physH = GetDeviceCaps(dc, PHYSICALHEIGHT);
  const int offX = GetDeviceCaps(dc, PHYSICALOFFSETX), offY = GetDeviceCaps(dc, PHYSICALOFFSETY);
  const int printW = GetDeviceCaps(dc, HORZRES), printH = GetDeviceCaps(dc, VERTRES);
  const int dpiX = GetDeviceCaps(dc, LOGPIXELSX), dpiY = GetDeviceCaps(dc, LOGPIXELSY);

  double w = FPDF_GetPageWidthF(page), h = FPDF_GetPageHeightF(page);
  int rotate = 0;
  // Match the page's orientation to the paper's.
  if ((w > h) != (printW > printH) && std::abs(w - h) > 1) {
    rotate = 1;
    std::swap(w, h);
  }
  double outW = w / 72.0 * dpiX, outH = h / 72.0 * dpiY;
  if (!g_actualSize) {
    double s = std::min(printW / outW, printH / outH);
    outW *= s;
    outH *= s;
  }
  // Center on the paper, but stay inside the printable area when fitting.
  int x = static_cast<int>(std::lround((physW - outW) / 2 - offX));
  int y = static_cast<int>(std::lround((physH - outH) / 2 - offY));
  if (!g_actualSize) {
    x = std::clamp(x, 0, std::max(0, printW - static_cast<int>(outW)));
    y = std::clamp(y, 0, std::max(0, printH - static_cast<int>(outH)));
  }
  FPDF_RenderPage(dc, page, x, y, static_cast<int>(std::lround(outW)), static_cast<int>(std::lround(outH)), rotate,
                  FPDF_ANNOT | FPDF_PRINTING);
}

}  // namespace

Outcome PrintDocument(HWND owner, Document& doc, int currentPage, const std::wstring& title,
                      const std::function<void(int, int)>& progress, std::wstring& message) {
  if (!doc.CanPrint()) {
    message = L"The author of this PDF doesn’t allow printing it.";
    return Outcome::NotAllowed;
  }
  const int pageCount = doc.PageCount();

  PROPSHEETPAGEW psp{sizeof(psp)};
  psp.hInstance = GetModuleHandleW(nullptr);
  psp.pszTemplate = MAKEINTRESOURCEW(IDD_PRINT_OPTIONS);
  psp.pfnDlgProc = OptionsProc;
  HPROPSHEETPAGE pages[1] = {CreatePropertySheetPageW(&psp)};

  PRINTPAGERANGE ranges[16] = {};
  PRINTDLGEXW pd{sizeof(pd)};
  pd.hwndOwner = owner;
  pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
  pd.nMaxPageRanges = 16;
  pd.lpPageRanges = ranges;
  pd.nMinPage = 1;
  pd.nMaxPage = static_cast<DWORD>(pageCount);
  pd.nCopies = 1;
  pd.nStartPage = START_PAGE_GENERAL;
  pd.nPropertyPages = pages[0] ? 1 : 0;
  pd.lphPropertyPages = pages[0] ? pages : nullptr;

  HRESULT hr = PrintDlgExW(&pd);
  auto cleanup = [&]() {
    if (pd.hDC) DeleteDC(pd.hDC);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
  };
  if (FAILED(hr) || pd.dwResultAction != PD_RESULT_PRINT || !pd.hDC) {
    cleanup();
    if (FAILED(hr)) {
      message = L"The print dialog couldn’t be opened. Is a printer installed?";
      return Outcome::Failed;
    }
    return Outcome::Cancelled;
  }

  std::vector<int> toPrint;
  if (pd.Flags & PD_PAGENUMS) {
    for (DWORD i = 0; i < pd.nPageRanges; ++i)
      for (DWORD p = ranges[i].nFromPage; p <= ranges[i].nToPage && p >= 1 && p <= static_cast<DWORD>(pageCount); ++p)
        toPrint.push_back(static_cast<int>(p) - 1);
  } else if (pd.Flags & PD_CURRENTPAGE) {
    toPrint.push_back(currentPage);
  } else {
    for (int i = 0; i < pageCount; ++i) toPrint.push_back(i);
  }

  PrintCopy copy;
  if (!copy.Create(doc)) {
    cleanup();
    message = L"This PDF couldn’t be prepared for printing.";
    return Outcome::Failed;
  }

  DOCINFOW di{sizeof(di)};
  di.lpszDocName = title.c_str();
  if (StartDocW(pd.hDC, &di) <= 0) {
    cleanup();
    message = L"The printer didn’t accept the document.";
    return Outcome::Failed;
  }
  bool ok = true;
  int done = 0;
  for (int index : toPrint) {
    if (progress) progress(done, static_cast<int>(toPrint.size()));
    FPDF_PAGE page = copy.FlatPage(index);
    if (StartPage(pd.hDC) <= 0) {
      ok = false;
      if (page) FPDF_ClosePage(page);
      break;
    }
    if (page) {
      RenderToPrinter(pd.hDC, page);
      FPDF_ClosePage(page);
    }
    if (EndPage(pd.hDC) <= 0) {
      ok = false;
      break;
    }
    ++done;
  }
  if (ok) EndDoc(pd.hDC);
  else AbortDoc(pd.hDC);
  cleanup();
  if (!ok) {
    message = L"Printing stopped because the printer reported an error.";
    return Outcome::Failed;
  }
  message = toPrint.size() == 1 ? L"Sent 1 page to the printer" : L"Sent " + std::to_wstring(toPrint.size()) + L" pages to the printer";
  return Outcome::Printed;
}

}  // namespace print
