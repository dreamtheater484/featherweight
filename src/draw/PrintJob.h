#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <functional>
#include <memory>
#include <string>

#include "draw/Painter.h"

namespace draw {

// Prints pages as vectors (plan §4.6): each page is drawn with a Painter into a Direct2D command
// list, and Direct2D's print control hands the pages to the printer through the XPS print path, so
// text and shapes stay sharp at any printer resolution. Pages are drawn in points, from the top
// left corner of the paper.
//
//   PrintJob job;
//   if (SUCCEEDED(job.Start(device, printer, devmode, L"Report.docx"))) {
//     for (...) job.AddPage(widthPt, heightPt, [&](Painter& p) { ... });
//     job.Finish();
//     job.Wait(60000);
//   }
class PrintJob {
 public:
  enum class Outcome { InProgress, Completed, Canceled, Failed };

  PrintJob();
  ~PrintJob();
  PrintJob(const PrintJob&) = delete;
  PrintJob& operator=(const PrintJob&) = delete;

  // `printer` is a printer's name and `devmode` its settings from the print dialog (null for the
  // printer's defaults). With `output`, the printer driver's output goes to that stream instead of
  // the printer's port: print to file, and tests.
  HRESULT Start(ID2D1Device* device, const std::wstring& printer, const DEVMODEW* devmode,
                const std::wstring& jobName, IStream* output = nullptr);
  HRESULT AddPage(float widthPt, float heightPt, const std::function<void(Painter&)>& draw);
  // Sends the last page. The spooler finishes the job in the background; Wait tells when.
  HRESULT Finish();
  // Waits until the job is completed, canceled or failed, or `timeoutMs` passes, handling window
  // messages meanwhile (status updates can arrive through them).
  Outcome Wait(DWORD timeoutMs);
  Outcome Status() const;
  int Pages() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace draw
