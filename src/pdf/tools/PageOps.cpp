// PDF syntax only, no text a person reads.  // lang: skip-file
#include "pdf/tools/PageOps.h"

#include <fpdf_edit.h>
#include <fpdf_formfill.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_signature.h>
#include <fpdf_transformpage.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <set>

#include "pdf/sign/PdfObjects.h"

namespace pdftools {

namespace {

// What a page inherits from its /Pages ancestors (ISO 32000-1, 7.7.3.4).
constexpr const char* kInheritable[] = {"Resources", "MediaBox", "CropBox", "Rotate"};
using Inherited = std::array<std::optional<pdfobj::Obj>, 4>;

struct Leaf {
  int num = 0, gen = 0;
  pdfobj::Obj dict;
  Inherited inherited;
};

// The page tree in page order, with what each page would inherit. False on anything unexpected
// (a loop, a kid that isn't a reference, too deep or too many).
bool WalkPages(pdfobj::File& f, int num, const Inherited& from, int depth, std::set<int>& seen, std::vector<Leaf>& leaves,
               int& branches) {
  if (depth > 64 || !seen.insert(num).second || leaves.size() > 1'000'000) return false;
  pdfobj::Obj node;
  int gen = 0;
  if (!f.Get(num, node, &gen) || node.type != pdfobj::Obj::Dict) return false;
  const pdfobj::Obj* kids = node.Get("Kids");
  const pdfobj::Obj* type = node.Get("Type");
  if (!kids || (type && type->IsName("Page"))) {
    leaves.push_back({num, gen, std::move(node), from});
    return true;
  }
  ++branches;
  Inherited mine = from;
  for (size_t k = 0; k < mine.size(); ++k)
    if (const pdfobj::Obj* v = node.Get(kInheritable[k])) mine[k] = *v;
  pdfobj::Obj list;
  if (!f.Resolve(*kids, list) || list.type != pdfobj::Obj::Array) return false;
  for (const pdfobj::Obj& kid : list.items)
    if (kid.type != pdfobj::Obj::Ref || !WalkPages(f, kid.num, mine, depth + 1, seen, leaves, branches)) return false;
  return true;
}

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

bool IsXfaForm(FPDF_DOCUMENT doc) {
  const int type = FPDF_GetFormType(doc);
  return type == FORMTYPE_XFA_FULL || type == FORMTYPE_XFA_FOREGROUND;
}

Block PageToolsBlock(FPDF_DOCUMENT doc, bool isXfa) {
  if (isXfa || IsXfaForm(doc)) return Block::Xfa;
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
  // What PDFium's functions can write on the page: its rotation, and its visible area (the crop
  // box within the media box) when the page doesn't have its own boxes. PinInherited does the rest.
  for (int p : pages) {
    FPDF_PAGE page = FPDF_LoadPage(doc, p);
    if (!page) continue;
    FPDFPage_SetRotation(page, FPDFPage_GetRotation(page));
    float l = 0, b = 0, r = 0, t = 0;
    FS_RECTF seen{};
    const bool media = FPDFPage_GetMediaBox(page, &l, &b, &r, &t);
    if (FPDF_GetPageBoundingBox(page, &seen)) {
      if (!media) FPDFPage_SetMediaBox(page, seen.left, seen.bottom, seen.right, seen.top);
      float cl = 0, cb = 0, cr = 0, ct = 0;
      const bool differs = std::fabs(seen.left - l) > 0.01f || std::fabs(seen.bottom - b) > 0.01f ||
                           std::fabs(seen.right - r) > 0.01f || std::fabs(seen.top - t) > 0.01f;
      if (media && differs && !FPDFPage_GetCropBox(page, &cl, &cb, &cr, &ct))
        FPDFPage_SetCropBox(page, seen.left, seen.bottom, seen.right, seen.top);
    }
    FPDF_ClosePage(page);
  }
  return FPDF_MovePages(doc, pages.data(), static_cast<unsigned long>(pages.size()), dest) != FALSE;
}

bool PinInherited(std::string_view bytes, const std::vector<int>& pages, int pageCount, std::string& out) {
  pdfobj::File f;
  if (!f.Open(bytes) || f.Trailer().Get("Encrypt")) return false;
  const pdfobj::Obj* root = f.Trailer().Get("Root");
  pdfobj::Obj catalog;
  if (!root || root->type != pdfobj::Obj::Ref || !f.Get(root->num, catalog) || catalog.type != pdfobj::Obj::Dict)
    return false;
  const pdfobj::Obj* tree = catalog.Get("Pages");
  if (!tree || tree->type != pdfobj::Obj::Ref) return false;
  std::vector<Leaf> leaves;
  std::set<int> seen;
  int branches = 0;
  if (!WalkPages(f, tree->num, Inherited{}, 0, seen, leaves, branches)) return false;
  // A tree PDFium reads otherwise (it repaired the file), or one branch, where a move keeps
  // every page's parent.
  if (static_cast<int>(leaves.size()) != pageCount || branches < 2) return false;
  std::vector<pdfobj::Update> updates;
  for (int p : SortedUnique(pages, pageCount)) {
    Leaf& leaf = leaves[static_cast<size_t>(p)];
    bool changed = false;
    for (size_t k = 0; k < leaf.inherited.size(); ++k) {
      if (leaf.inherited[k] && !leaf.dict.Get(kInheritable[k])) {
        leaf.dict.Set(kInheritable[k], *leaf.inherited[k]);
        changed = true;
      }
    }
    if (changed) updates.push_back({leaf.num, leaf.gen, pdfobj::Write(leaf.dict)});
  }
  return !updates.empty() && pdfobj::AppendUpdate(bytes, f, updates, f.Size(), out);
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

bool SaveDocument(FPDF_DOCUMENT doc, size_t loadedSize, std::string& out) {
  if (FPDF_GetSignatureCount(doc) <= 0) return SaveBytes(doc, false, out);
  if (!SaveBytes(doc, true, out)) return false;
  if (loadedSize > 0 && out.size() > loadedSize) TypeLastXrefStream(out, loadedSize);
  return true;
}

bool TypeLastXrefStream(std::string& bytes, size_t from) {
  const size_t sx = bytes.rfind("startxref");
  if (sx == std::string::npos) return false;
  size_t at = sx + 9;
  while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\r' || bytes[at] == '\n')) ++at;
  size_t offset = 0;
  bool digits = false;
  for (; at < bytes.size() && bytes[at] >= '0' && bytes[at] <= '9'; ++at, digits = true) {
    if (offset > (size_t{1} << 40)) return false;
    offset = offset * 10 + static_cast<size_t>(bytes[at] - '0');
  }
  if (!digits || offset < from || offset >= sx) return false;
  // "num gen obj", then the stream's dictionary.
  const size_t obj = bytes.find("obj", offset);
  if (obj == std::string::npos || obj > offset + 32) return false;
  size_t dict = obj + 3;
  while (dict < sx && (bytes[dict] == ' ' || bytes[dict] == '\r' || bytes[dict] == '\n')) ++dict;
  if (bytes.compare(dict, 2, "<<") != 0) return false;
  size_t end = dict;
  pdfobj::Obj o;
  if (!pdfobj::ParseObject(std::string_view(bytes).substr(0, sx), end, o) || o.type != pdfobj::Obj::Stream ||
      !o.Get("W") || o.Get("Type"))
    return false;
  bytes.insert(dict + 2, "/Type /XRef ");
  return true;
}

}  // namespace pdftools
