#include "pdf/tools/PageOps.h"

#include <fpdf_edit.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>

#include <algorithm>

namespace pdftools {

namespace {

// PDF permission bits (ISO 32000-1, table 22).
constexpr unsigned long kModify = 1u << 3, kCopy = 1u << 4, kAssemble = 1u << 10;

bool Encrypted(FPDF_DOCUMENT doc) { return FPDF_GetSecurityHandlerRevision(doc) >= 0; }

// Changing pages needs "modify" and, from revision 3, "assemble" (revision 2 had no such bit).
bool MayChangePages(FPDF_DOCUMENT doc) {
  if (!Encrypted(doc)) return true;
  const unsigned long p = FPDF_GetDocPermissions(doc);
  if (!(p & kModify)) return false;
  return FPDF_GetSecurityHandlerRevision(doc) < 3 || (p & kAssemble);
}

std::vector<int> SortedUnique(std::vector<int> pages, int count) {
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  pages.erase(std::remove_if(pages.begin(), pages.end(), [count](int p) { return p < 0 || p >= count; }), pages.end());
  return pages;
}

struct MemoryWriter : FPDF_FILEWRITE {
  std::string* bytes = nullptr;
};

int WriteToMemory(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
  static_cast<MemoryWriter*>(self)->bytes->append(static_cast<const char*>(data), size);
  return 1;
}

}  // namespace

Block PageToolsBlock(FPDF_DOCUMENT doc, bool isXfa) {
  if (isXfa) return Block::Xfa;
  if (!MayChangePages(doc)) return Block::Protected;
  return Block::None;
}

bool Rotate(FPDF_DOCUMENT doc, const std::vector<int>& pages, int quarterTurns) {
  const int count = FPDF_GetPageCount(doc);
  const std::vector<int> list = SortedUnique(pages, count);
  if (list.empty()) return false;
  const int turn = ((quarterTurns % 4) + 4) % 4;
  if (turn == 0) return true;
  bool ok = true;
  for (int i : list) {
    FPDF_PAGE page = FPDF_LoadPage(doc, i);
    if (!page) {
      ok = false;
      continue;
    }
    const int now = std::max(0, FPDFPage_GetRotation(page));
    FPDFPage_SetRotation(page, (now + turn) % 4);
    FPDF_ClosePage(page);
  }
  return ok;
}

bool Delete(FPDF_DOCUMENT doc, std::vector<int> pages) {
  const int count = FPDF_GetPageCount(doc);
  pages = SortedUnique(std::move(pages), count);
  if (pages.empty() || static_cast<int>(pages.size()) >= count) return false;
  // From the last, so the indices still to go stay where they were.
  for (auto it = pages.rbegin(); it != pages.rend(); ++it) FPDFPage_Delete(doc, *it);
  return FPDF_GetPageCount(doc) == count - static_cast<int>(pages.size());
}

int MoveDestination(const std::vector<int>& sortedPages, int gap) {
  int before = 0;
  for (int p : sortedPages)
    if (p < gap) ++before;
  return gap - before;
}

bool Move(FPDF_DOCUMENT doc, std::vector<int> pages, int gap) {
  const int count = FPDF_GetPageCount(doc);
  pages = SortedUnique(std::move(pages), count);
  if (pages.empty() || gap < 0 || gap > count) return false;
  const int dest = MoveDestination(pages, gap);
  // Already there: the pages are a run that starts at the destination.
  bool same = true;
  for (size_t k = 0; k < pages.size(); ++k) same = same && pages[k] == dest + static_cast<int>(k);
  if (same) return true;
  return FPDF_MovePages(doc, pages.data(), static_cast<unsigned long>(pages.size()), dest) != FALSE;
}

Source::~Source() {
  if (doc) FPDF_CloseDocument(doc);
}

SourceError OpenSource(std::string bytes, const std::string& password, Source& out) {
  if (bytes.empty()) return SourceError::Read;
  out.bytes = std::move(bytes);
  out.doc = FPDF_LoadMemDocument64(out.bytes.data(), out.bytes.size(), password.empty() ? nullptr : password.c_str());
  if (!out.doc) {
    const unsigned long err = FPDF_GetLastError();
    return err == FPDF_ERR_PASSWORD ? SourceError::Password : SourceError::NotPdf;
  }
  if (Encrypted(out.doc) && !(FPDF_GetDocPermissions(out.doc) & kCopy)) return SourceError::NotAllowed;
  if (FPDF_GetPageCount(out.doc) <= 0) return SourceError::Empty;
  return SourceError::None;
}

int Insert(FPDF_DOCUMENT doc, const Source& src, int at) {
  if (!src.doc) return 0;
  const int count = FPDF_GetPageCount(doc), adding = FPDF_GetPageCount(src.doc);
  if (adding <= 0) return 0;
  at = std::clamp(at, 0, count);
  if (!FPDF_ImportPagesByIndex(doc, src.doc, nullptr, 0, at)) return 0;
  return FPDF_GetPageCount(doc) - count;
}

bool Extract(FPDF_DOCUMENT doc, const std::vector<int>& pages, std::string& out) {
  const int count = FPDF_GetPageCount(doc);
  std::vector<int> list;
  for (int p : pages)
    if (p >= 0 && p < count) list.push_back(p);
  if (list.empty()) return false;
  FPDF_DOCUMENT part = FPDF_CreateNewDocument();
  if (!part) return false;
  bool ok = FPDF_ImportPagesByIndex(part, doc, list.data(), static_cast<unsigned long>(list.size()), 0) != FALSE;
  if (ok) {
    FPDF_CopyViewerPreferences(part, doc);
    ok = SaveBytes(part, false, out);
  }
  FPDF_CloseDocument(part);
  return ok;
}

std::vector<std::vector<int>> SplitEvery(int pageCount, int every) {
  std::vector<std::vector<int>> parts;
  if (pageCount <= 0 || every <= 0) return parts;
  for (int first = 0; first < pageCount; first += every) {
    std::vector<int> part;
    for (int p = first; p < std::min(pageCount, first + every); ++p) part.push_back(p);
    parts.push_back(std::move(part));
  }
  return parts;
}

std::vector<std::vector<int>> SplitAt(int pageCount, std::vector<int> starts) {
  starts = SortedUnique(std::move(starts), pageCount);
  if (starts.empty() || starts.front() != 0) starts.insert(starts.begin(), 0);
  std::vector<std::vector<int>> parts;
  if (pageCount <= 0) return parts;
  for (size_t k = 0; k < starts.size(); ++k) {
    const int end = k + 1 < starts.size() ? starts[k + 1] : pageCount;
    std::vector<int> part;
    for (int p = starts[k]; p < end; ++p) part.push_back(p);
    parts.push_back(std::move(part));
  }
  return parts;
}

std::wstring PageList(const std::vector<int>& pages) {
  std::vector<int> list = pages;
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());
  std::wstring out;
  for (size_t k = 0; k < list.size();) {
    size_t run = k;
    while (run + 1 < list.size() && list[run + 1] == list[run] + 1) ++run;
    if (!out.empty()) out += L", ";
    out += std::to_wstring(list[k] + 1);
    if (run > k) out += L"-" + std::to_wstring(list[run] + 1);
    k = run + 1;
  }
  return out;
}

bool SaveBytes(FPDF_DOCUMENT doc, bool incremental, std::string& out) {
  MemoryWriter w;
  w.version = 1;
  w.WriteBlock = WriteToMemory;
  w.bytes = &out;
  out.clear();
  if (incremental && FPDF_SaveAsCopy(doc, &w, FPDF_INCREMENTAL)) return true;
  out.clear();
  return FPDF_SaveAsCopy(doc, &w, FPDF_NO_INCREMENTAL) != FALSE;
}

}  // namespace pdftools
