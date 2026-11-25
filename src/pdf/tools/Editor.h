#pragma once

// A PDF open for the page tools, OCR and signing without a window (F1, f1-pdf): what fwtool's PDF
// torture and OCR runs call (f1-gate), through the same functions as the PDF tab, with the same
// rules: moves pin what pages inherit, an edited PDF is saved whole and a signed one incrementally,
// an unedited one is its bytes as they were. PDFium must be initialised; like PDFium, one thread.

#include <windows.h>
#include <wincrypt.h>

#include <fpdfview.h>

#include <memory>
#include <string>
#include <vector>

#include "pdf/ocr/Ocr.h"
#include "pdf/sign/Sign.h"
#include "pdf/sign/Stamp.h"
#include "pdf/tools/PageOps.h"

namespace fw::pkg {
struct SaveFaults;
}

namespace pdftools {

class Editor {
 public:
  Editor() = default;
  Editor(const Editor&) = delete;
  Editor& operator=(const Editor&) = delete;
  ~Editor();

  // Opens a PDF's bytes; `password` for a protected one. None, Read (empty), NotPdf or Password.
  SourceError Open(std::string bytes, const std::string& password = {});
  FPDF_DOCUMENT Handle() const { return doc_; }
  int PageCount() const;
  bool Modified() const { return modified_; }

  // Why the page tools may not change this PDF (None: they may), and why it can't be signed
  // (Error::None: it can; Encrypted for a protected PDF or an XFA form).
  Block PageToolsBlocked() const;
  pdfsign::Error SigningBlocked() const;

  // The page tools, as the Pages panel does them. Each refuses (false, 0) when PageToolsBlocked.
  bool Rotate(const std::vector<int>& pages, int quarterTurns);
  bool Delete(const std::vector<int>& pages);
  bool Move(const std::vector<int>& pages, int gap);  // pins inherited attributes first
  int Insert(const Source& src, int at);              // the pages inserted; `src` may close after
  bool Extract(const std::vector<int>& pages, std::string& out) const;

  // Recognize text (OCR) on `pages`, or every page when empty; pages that already have text are
  // left alone unless `force`. Ok with `words` written in all; otherwise the first failure
  // (NoEngine, NoLanguage: nothing done) or Failed for a page that couldn't be read.
  pdfocr::Status Recognize(const std::vector<int>& pages, int& words, bool force = false);

  // A drawn signature `widthPt` wide, centred on (x, y) in page points, as a stamp annotation.
  bool PlaceSignature(int page, float x, float y, float widthPt, const pdfsign::Ink& ink);
  // A certificate signature (PAdES B-B, invisible, on the first page) after the PDF as it is now;
  // the document is the signed one afterwards (not modified: what Bytes gives is what was signed).
  pdfsign::Error Sign(const CERT_CONTEXT* cert, const pdfsign::Details& details);

  // The bytes a save writes: as opened (or signed) when nothing changed since, otherwise whole,
  // or incrementally when the PDF is signed.
  bool Bytes(std::string& out);
  // Writes Bytes to `path` safely (pkg::SafeWrite: a temporary file, flushed, then put in place),
  // as the tab's save does without its backups. `faults` for the never-damage tests.
  bool SaveTo(const std::wstring& path, std::wstring& error, const fw::pkg::SaveFaults* faults = nullptr);

 private:
  bool Reload(std::string bytes, bool modified);

  // Copies of sources pages were inserted from, kept only when the document couldn't be opened
  // again after the insert (its imported pages read from them). Before doc_: they close after it.
  std::vector<std::unique_ptr<Source>> kept_;
  std::string bytes_;  // PDFium reads from these while the document is open
  std::string password_;
  FPDF_DOCUMENT doc_ = nullptr;
  bool modified_ = false;
  bool branched_ = false;  // HasPageBranches(bytes_) when they were opened: moves may need pinning
};

}  // namespace pdftools
