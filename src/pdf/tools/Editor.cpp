#include "pdf/tools/Editor.h"

#include <fpdf_signature.h>

#include <algorithm>
#include <span>

#include "core/Strings.h"
#include "pdf/ocr/TextLayer.h"
#include "pkg/SafeFile.h"

namespace pdftools {

Editor::~Editor() {
  if (doc_) FPDF_CloseDocument(doc_);
}

SourceError Editor::Open(std::string bytes, const std::string& password) {
  if (doc_) FPDF_CloseDocument(doc_);
  doc_ = nullptr;
  modified_ = false;
  if (bytes.empty()) return SourceError::Read;
  bytes_ = std::move(bytes);
  password_ = password;
  doc_ = FPDF_LoadMemDocument64(bytes_.data(), bytes_.size(), password_.empty() ? nullptr : password_.c_str());
  if (!doc_) return FPDF_GetLastError() == FPDF_ERR_PASSWORD ? SourceError::Password : SourceError::NotPdf;
  return SourceError::None;
}

int Editor::PageCount() const { return doc_ ? FPDF_GetPageCount(doc_) : 0; }

Block Editor::PageToolsBlocked() const { return doc_ ? PageToolsBlock(doc_) : Block::Protected; }

pdfsign::Error Editor::SigningBlocked() const {
  if (!doc_) return pdfsign::Error::Unreadable;
  if (IsXfaForm(doc_) || FPDF_GetSecurityHandlerRevision(doc_) >= 0) return pdfsign::Error::Encrypted;
  return pdfsign::Error::None;
}

bool Editor::Reload(std::string bytes, bool modified) {
  FPDF_DOCUMENT fresh =
      FPDF_LoadMemDocument64(bytes.data(), bytes.size(), password_.empty() ? nullptr : password_.c_str());
  if (!fresh) return false;
  FPDF_CloseDocument(doc_);
  doc_ = fresh;
  bytes_ = std::move(bytes);  // after the old document is closed: it read from the old bytes
  modified_ = modified;
  return true;
}

bool Editor::Rotate(const std::vector<int>& pages, int quarterTurns) {
  if (PageToolsBlocked() != Block::None || !pdftools::Rotate(doc_, pages, quarterTurns)) return false;
  modified_ = true;
  return true;
}

bool Editor::Delete(const std::vector<int>& pages) {
  if (PageToolsBlocked() != Block::None || !pdftools::Delete(doc_, pages)) return false;
  modified_ = true;
  return true;
}

bool Editor::Move(const std::vector<int>& pages, int gap) {
  if (PageToolsBlocked() != Block::None) return false;
  std::string now, pinned;
  if (Bytes(now) && PinInherited(now, pages, PageCount(), pinned)) Reload(std::move(pinned), true);
  if (!pdftools::Move(doc_, pages, gap)) return false;
  modified_ = true;
  return true;
}

int Editor::Insert(const Source& src, int at) {
  if (PageToolsBlocked() != Block::None) return 0;
  const int added = pdftools::Insert(doc_, src, at);
  if (added > 0) modified_ = true;
  return added;
}

bool Editor::Extract(const std::vector<int>& pages, std::string& out) const {
  return doc_ && pdftools::Extract(doc_, pages, out);
}

pdfocr::Status Editor::Recognize(const std::vector<int>& pagesIn, int& words, bool force) {
  words = 0;
  if (PageToolsBlocked() != Block::None) return pdfocr::Status::Failed;
  std::vector<int> pages = pagesIn;
  if (pages.empty())
    for (int i = 0; i < PageCount(); ++i) pages.push_back(i);
  pdfocr::Status worst = pdfocr::Status::Ok;
  for (int p : pages) {
    FPDF_PAGE page = FPDF_LoadPage(doc_, p);
    if (!page) {
      worst = pdfocr::Status::Failed;
      continue;
    }
    int written = 0;
    const pdfocr::Status s = pdfocr::RecognizePage(doc_, page, written, force);
    FPDF_ClosePage(page);
    words += written;
    if (written > 0) modified_ = true;
    if (s == pdfocr::Status::NoEngine || s == pdfocr::Status::NoLanguage) return s;
    if (s != pdfocr::Status::Ok) worst = s;
  }
  return worst;
}

bool Editor::PlaceSignature(int pageIndex, float x, float y, float widthPt, const pdfsign::Ink& ink) {
  if (PageToolsBlocked() != Block::None || ink.Empty()) return false;
  FPDF_PAGE page = FPDF_LoadPage(doc_, pageIndex);
  if (!page) return false;
  const FS_RECTF rect = pdfsign::StampRect(page, x, y, widthPt, ink.Aspect());
  const bool ok = pdfsign::AddSignatureStamp(doc_, page, rect, ink) >= 0;
  FPDF_ClosePage(page);
  if (ok) modified_ = true;
  return ok;
}

pdfsign::Error Editor::Sign(const CERT_CONTEXT* cert, const pdfsign::Details& details) {
  if (const pdfsign::Error why = SigningBlocked(); why != pdfsign::Error::None) return why;
  std::string now, signedBytes;
  if (!Bytes(now)) return pdfsign::Error::Failed;
  const pdfsign::Error e = pdfsign::SignPdf(now, cert, details, signedBytes);
  if (e != pdfsign::Error::None) return e;
  return Reload(std::move(signedBytes), false) ? pdfsign::Error::None : pdfsign::Error::Failed;
}

bool Editor::Bytes(std::string& out) {
  if (!doc_) return false;
  if (!modified_) {
    out = bytes_;
    return true;
  }
  return SaveDocument(doc_, bytes_.size(), out);
}

bool Editor::SaveTo(const std::wstring& path, std::wstring& error, const fw::pkg::SaveFaults* faults) {
  std::string bytes;
  if (!Bytes(bytes)) {
    error = L"The PDF couldn't be written.";  // lang: no (fwtool's, in English)
    return false;
  }
  const fw::pkg::SaveResult r = fw::pkg::SafeWrite(
      path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), faults);
  if (!r.Ok()) {
    error = fw::Utf8ToWide(r.message);
    return false;
  }
  return true;
}

}  // namespace pdftools
