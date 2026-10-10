#include "Document.h"

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>

#include <algorithm>
#include <cmath>

#include "FormFill.h"
#include "Util.h"
#include "core/Strings.h"

namespace {

bool g_libraryReady = false;
bool g_libraryFailed = false;

// Files up to this size are read into memory, so the file on disk is never locked.
constexpr unsigned long long kInMemoryLimit = 256ull << 20;

int GetBlock(void* param, unsigned long position, unsigned char* buf, unsigned long size) {
  HANDLE h = static_cast<HANDLE>(param);
  OVERLAPPED ov{};
  ov.Offset = position;
  DWORD read = 0;
  if (!ReadFile(h, buf, size, &read, &ov)) return 0;
  return read == size ? 1 : 0;
}

std::wstring FriendlyOpenError(DWORD err) {
  switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
      return L"the file no longer exists.";
    case ERROR_ACCESS_DENIED:
      return L"you don't have permission to open it.";
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return L"another program is using it. Close that program and try again.";
    default:
      return L"Windows reported: " + util::LastErrorMessage(err);
  }
}

std::string Latin1(const std::wstring& s) {
  int n = WideCharToMultiByte(1252, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(1252, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

struct FileWriter : FPDF_FILEWRITE {
  HANDLE h = INVALID_HANDLE_VALUE;
  bool ok = true;
};

int WriteBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size) {
  auto* w = static_cast<FileWriter*>(self);
  DWORD written = 0;
  if (!WriteFile(w->h, data, size, &written, nullptr) || written != size) w->ok = false;
  return w->ok ? 1 : 0;
}

// Replaces `target` with `source` even while another handle (with FILE_SHARE_DELETE) has it open.
bool PosixReplace(const std::wstring& source, const std::wstring& target) {
  HANDLE h = CreateFileW(source.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  size_t bytes = sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t);
  std::vector<unsigned char> buf(bytes, 0);
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
  info->Flags = 0x1 /*FILE_RENAME_FLAG_REPLACE_IF_EXISTS*/ | 0x2 /*FILE_RENAME_FLAG_POSIX_SEMANTICS*/;
  info->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
  memcpy(info->FileName, target.c_str(), info->FileNameLength);
  BOOL ok = SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(22) /*FileRenameInfoEx*/, info,
                                       static_cast<DWORD>(bytes));
  CloseHandle(h);
  return ok != FALSE;
}

// PDF drawing commands for the note icon: a 20 x 20 pt rounded speech bubble with its top-left at (x, y).
std::wstring NoteAppearance(float x, float y) {
  const float l = x + 1.0f, r = x + 19.0f, t = y - 1.0f, b = y - 15.0f, rad = 4.0f, k = rad * 0.5523f;
  wchar_t buf[2048];
  swprintf_s(buf,
             L"q 1 0.725 0 rg "
             L"%.2f %.2f m %.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c "
             L"%.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c h "
             L"%.2f %.2f m %.2f %.2f l %.2f %.2f l h f "
             L"1 1 1 rg %.2f %.2f 11 1.6 re f %.2f %.2f 11 1.6 re f %.2f %.2f 7 1.6 re f Q",
             // rounded rectangle
             l + rad, b, r - rad, b, r - rad + k, b, r, b + rad - k, r, b + rad, r, t - rad, r, t - rad + k, r - rad + k,
             t, r - rad, t, l + rad, t, l + rad - k, t, l, t - rad + k, l, t - rad, l, b + rad, l, b + rad - k,
             l + rad - k, b, l + rad, b,
             // tail
             l + 3.0f, b + 0.5f, l + 3.5f, b - 4.0f, l + 8.0f, b + 0.5f,
             // text lines
             l + 3.5f, t - 5.0f, l + 3.5f, t - 8.4f, l + 3.5f, t - 11.8f);
  return buf;
}

}  // namespace

void Document::InitLibrary() {
  if (g_libraryReady || g_libraryFailed) return;
  // pdfium.dll is delay-loaded; make sure it is the copy next to the exe and that it exists.
  std::wstring dll = util::ExeDir() + L"\\pdfium.dll";
  if (!LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
    g_libraryFailed = true;
    return;
  }
  FPDF_LIBRARY_CONFIG config{};
  config.version = 2;
  config.m_pUserFontPaths = nullptr;
  config.m_pIsolate = nullptr;
  config.m_v8EmbedderSlot = 0;
  FPDF_InitLibraryWithConfig(&config);
  g_libraryReady = true;
}

bool Document::LibraryAvailable() {
  InitLibrary();
  return g_libraryReady;
}

OpenResult Document::Open(const std::wstring& path, const std::wstring& password, std::unique_ptr<Document>& out,
                          std::wstring& error) {
  const std::wstring name = L"“" + fw::FileNameOf(path) + L"”";
  if (!LibraryAvailable()) {
    error = L"Featherweight's PDF engine (pdfium.dll) is missing. Please reinstall Featherweight.";
    return OpenResult::Failed;
  }
  std::unique_ptr<Document> d(new Document());
  d->path_ = path;

  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    error = name + L" couldn't be opened — " + FriendlyOpenError(GetLastError());
    return OpenResult::Failed;
  }
  LARGE_INTEGER size{};
  GetFileSizeEx(h, &size);
  if (size.QuadPart == 0) {
    CloseHandle(h);
    error = name + L" couldn't be opened — the file is empty.";
    return OpenResult::Failed;
  }
  if (static_cast<unsigned long long>(size.QuadPart) >= 0xFFFFFFFFull) {
    CloseHandle(h);
    error = name + L" is too large to open (more than 4 GB).";
    return OpenResult::Failed;
  }

  if (static_cast<unsigned long long>(size.QuadPart) <= kInMemoryLimit) {
    d->memory_.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    bool ok = true;
    while (ok && done < d->memory_.size()) {
      DWORD chunk = static_cast<DWORD>(std::min<size_t>(d->memory_.size() - done, 16u << 20));
      DWORD read = 0;
      ok = ReadFile(h, d->memory_.data() + done, chunk, &read, nullptr) && read > 0;
      done += read;
    }
    CloseHandle(h);
    if (!ok) {
      error = name + L" couldn't be read completely.";
      return OpenResult::Failed;
    }
  } else {
    d->file_ = h;
    d->access_.m_FileLen = static_cast<unsigned long>(size.QuadPart);
    d->access_.m_GetBlock = GetBlock;
    d->access_.m_Param = h;
  }

  auto load = [&](const std::string& pw) -> FPDF_DOCUMENT {
    const char* p = pw.empty() ? nullptr : pw.c_str();
    if (!d->memory_.empty()) return FPDF_LoadMemDocument64(d->memory_.data(), d->memory_.size(), p);
    return FPDF_LoadCustomDocument(&d->access_, p);
  };

  d->doc_ = load(fw::WideToUtf8(password));
  unsigned long err = d->doc_ ? FPDF_ERR_SUCCESS : FPDF_GetLastError();
  if (!d->doc_ && err == FPDF_ERR_PASSWORD && !password.empty()) {
    std::string legacy = Latin1(password);  // older encryption expects Latin-1 passwords
    if (legacy != fw::WideToUtf8(password)) {
      d->doc_ = load(legacy);
      err = d->doc_ ? FPDF_ERR_SUCCESS : FPDF_GetLastError();
    }
  }
  if (!d->doc_) {
    switch (err) {
      case FPDF_ERR_PASSWORD:
        return password.empty() ? OpenResult::NeedPassword : OpenResult::WrongPassword;
      case FPDF_ERR_SECURITY:
        error = name + L" uses a kind of protection Featherweight doesn't support.";
        break;
      case FPDF_ERR_FILE:
        error = name + L" couldn't be opened — the file couldn't be read.";
        break;
      default:
        error = name + L" couldn't be opened — it seems to be damaged or isn't a PDF.";
        break;
    }
    return OpenResult::Failed;
  }

  d->RefreshPageSizes();
  out = std::move(d);
  return OpenResult::Ok;
}

void Document::RefreshPageSizes() {
  int n = std::max(0, FPDF_GetPageCount(doc_));
  // Pages that are already loaded keep their slots; new pages start unloaded.
  if (static_cast<int>(pages_.size()) > n) {
    for (int i = n; i < static_cast<int>(pages_.size()); ++i) ClosePage(i);
  }
  pages_.resize(n, nullptr);
  textPages_.resize(n, nullptr);
  webLinks_.resize(n);
  lastUse_.resize(n, 0);
  sizes_.assign(n, fw::PageSizePt{612, 792});
  for (int i = 0; i < n; ++i) {
    FS_SIZEF s{};
    if (FPDF_GetPageSizeByIndexF(doc_, i, &s) && s.width > 0 && s.height > 0) sizes_[i] = {s.width, s.height};
  }
}

Document::~Document() {
  for (int i = 0; i < static_cast<int>(pages_.size()); ++i) ClosePage(i);
  if (form_) FPDFDOC_ExitFormFillEnvironment(form_);
  env_.reset();
  if (doc_) FPDF_CloseDocument(doc_);
  if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

void Document::AttachHost(DocumentHost* host, bool highlightFields) {
  host_ = host;
  if (form_) return;
  env_ = std::make_unique<FormEnv>(this);
  form_ = FPDFDOC_InitFormFillEnvironment(doc_, env_.get());
  int type = FPDF_GetFormType(doc_);
  hasForm_ = type != FORMTYPE_NONE;
#ifdef PDF_ENABLE_XFA
  if (type == FORMTYPE_XFA_FULL || type == FORMTYPE_XFA_FOREGROUND) {
    isXfa_ = FPDF_LoadXFA(doc_) != FALSE;
    if (isXfa_) RefreshPageSizes();
  }
#endif
  if (!form_) return;
  SetHighlightFields(highlightFields);
  FORM_DoDocumentJSAction(form_);
  FORM_DoDocumentOpenAction(form_);
}

void Document::SetHighlightFields(bool on) {
  if (!form_) return;
  if (on) {
    // PDFium takes this color as 0x00BBGGRR; this is a soft blue (#D6E4FF).
    FPDF_SetFormFieldHighlightColor(form_, FPDF_FORMFIELD_UNKNOWN, 0xFFE4D6);
    FPDF_SetFormFieldHighlightAlpha(form_, 110);
  } else {
    FPDF_RemoveFormFieldHighlight(form_);
  }
}

bool Document::CanPrint() const {
  if (FPDF_GetSecurityHandlerRevision(doc_) < 0) return true;  // not encrypted
  return (FPDF_GetDocPermissions(doc_) & 0x4) != 0;
}

FPDF_PAGE Document::Page(int index) {
  if (index < 0 || index >= static_cast<int>(pages_.size())) return nullptr;
  lastUse_[index] = ++useCounter_;
  if (!pages_[index]) {
    FPDF_PAGE page = FPDF_LoadPage(doc_, index);
    if (!page) return nullptr;
    pages_[index] = page;
    if (form_) {
      FORM_OnAfterLoadPage(page, form_);
      FORM_DoPageAAction(page, form_, FPDFPAGE_AACTION_OPEN);
    }
  }
  return pages_[index];
}

FPDF_PAGE Document::PageIfLoaded(int index) const {
  if (index < 0 || index >= static_cast<int>(pages_.size())) return nullptr;
  return pages_[index];
}

int Document::IndexOfPage(FPDF_PAGE page) const {
  if (!page) return -1;
  for (int i = 0; i < static_cast<int>(pages_.size()); ++i)
    if (pages_[i] == page) return i;
  return -1;
}

FPDF_TEXTPAGE Document::TextPage(int index) {
  FPDF_PAGE page = Page(index);
  if (!page) return nullptr;
  if (!textPages_[index]) textPages_[index] = FPDFText_LoadPage(page);
  return textPages_[index];
}

void Document::ClosePage(int index) {
  if (index < 0 || index >= static_cast<int>(pages_.size())) return;
  webLinks_[index].reset();
  if (textPages_[index]) {
    FPDFText_ClosePage(textPages_[index]);
    textPages_[index] = nullptr;
  }
  if (pages_[index]) {
    if (form_) FORM_OnBeforeClosePage(pages_[index], form_);
    FPDF_ClosePage(pages_[index]);
    pages_[index] = nullptr;
  }
}

int Document::LoadedPageCount() const {
  return static_cast<int>(std::count_if(pages_.begin(), pages_.end(), [](FPDF_PAGE p) { return p != nullptr; }));
}

void Document::TrimPages(int keepFirst, int keepLast, int pinned) {
  constexpr int kExtraLoaded = 12;  // recently used pages kept around for quick back-and-forth
  std::vector<int> candidates;
  for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
    if (!pages_[i] || (i >= keepFirst && i <= keepLast) || i == pinned) continue;
    candidates.push_back(i);
  }
  if (static_cast<int>(candidates.size()) <= kExtraLoaded) return;
  std::sort(candidates.begin(), candidates.end(), [&](int a, int b) { return lastUse_[a] < lastUse_[b]; });
  for (size_t k = 0; k + kExtraLoaded < candidates.size(); ++k) ClosePage(candidates[k]);
}

LinkTarget Document::DestToTarget(FPDF_DEST dest) {
  LinkTarget t;
  if (!dest) return t;
  int page = FPDFDest_GetDestPageIndex(doc_, dest);
  if (page < 0 || page >= PageCount()) return t;
  t.kind = LinkTarget::Kind::Page;
  t.page = page;
  FPDF_BOOL hasX = 0, hasY = 0, hasZoom = 0;
  FS_FLOAT x = 0, y = 0, zoom = 0;
  if (FPDFDest_GetLocationInPage(dest, &hasX, &hasY, &hasZoom, &x, &y, &zoom) && hasY) {
    t.hasPos = true;
    t.x = hasX ? x : 0;
    t.y = y;
  }
  return t;
}

LinkTarget Document::LinkAt(int pageIndex, double x, double y) {
  LinkTarget t;
  FPDF_PAGE page = Page(pageIndex);
  if (!page) return t;
  if (FPDF_LINK link = FPDFLink_GetLinkAtPoint(page, x, y)) {
    FS_RECTF rect{};
    FPDFLink_GetAnnotRect(link, &rect);
    if (FPDF_DEST dest = FPDFLink_GetDest(doc_, link)) {
      t = DestToTarget(dest);
    } else if (FPDF_ACTION action = FPDFLink_GetAction(link)) {
      switch (FPDFAction_GetType(action)) {
        case PDFACTION_GOTO: t = DestToTarget(FPDFAction_GetDest(doc_, action)); break;
        case PDFACTION_URI: {
          unsigned long len = FPDFAction_GetURIPath(doc_, action, nullptr, 0);
          if (len > 1) {
            std::string buf(len, '\0');
            FPDFAction_GetURIPath(doc_, action, buf.data(), len);
            buf.resize(strnlen(buf.c_str(), buf.size()));
            t.kind = LinkTarget::Kind::Uri;
            t.uri = fw::Utf8ToWide(buf);
          }
          break;
        }
        case PDFACTION_REMOTEGOTO:
        case PDFACTION_LAUNCH:
        case PDFACTION_EMBEDDEDGOTO: t.kind = LinkTarget::Kind::Blocked; break;
        default: break;
      }
    }
    t.rect = rect;
    if (t.kind != LinkTarget::Kind::None) return t;
  }
  for (const auto& wl : WebLinks(pageIndex)) {
    for (const auto& r : wl.rects) {
      if (x >= r.left && x <= r.right && y >= r.bottom && y <= r.top) {
        t.kind = LinkTarget::Kind::Uri;
        t.uri = wl.url;
        t.rect = r;
        return t;
      }
    }
  }
  return t;
}

const std::vector<Document::WebLink>& Document::WebLinks(int index) {
  static const std::vector<WebLink> empty;
  if (index < 0 || index >= PageCount()) return empty;
  if (!webLinks_[index]) {
    auto list = std::make_unique<std::vector<WebLink>>();
    if (FPDF_TEXTPAGE tp = TextPage(index)) {
      if (FPDF_PAGELINK pl = FPDFLink_LoadWebLinks(tp)) {
        int count = FPDFLink_CountWebLinks(pl);
        for (int i = 0; i < count; ++i) {
          WebLink wl;
          int len = FPDFLink_GetURL(pl, i, nullptr, 0);
          if (len > 1) {
            std::wstring url(static_cast<size_t>(len), L'\0');
            FPDFLink_GetURL(pl, i, reinterpret_cast<unsigned short*>(url.data()), len);
            url.resize(wcsnlen(url.c_str(), url.size()));
            wl.url = url;
          }
          int rects = FPDFLink_CountRects(pl, i);
          for (int r = 0; r < rects; ++r) {
            double l, t, rr, b;
            if (FPDFLink_GetRect(pl, i, r, &l, &t, &rr, &b))
              wl.rects.push_back(FS_RECTF{static_cast<float>(l), static_cast<float>(t), static_cast<float>(rr),
                                          static_cast<float>(b)});
          }
          if (!wl.url.empty()) list->push_back(std::move(wl));
        }
        FPDFLink_CloseWebLinks(pl);
      }
    }
    webLinks_[index] = std::move(list);
  }
  return *webLinks_[index];
}

bool Document::FocusedField(int& page) const {
  if (!form_) return false;
  int index = -1;
  FPDF_ANNOTATION annot = nullptr;
  if (!FORM_GetFocusedAnnot(form_, &index, &annot)) return false;
  if (annot) FPDFPage_CloseAnnot(annot);
  page = index;
  return index >= 0;
}

int Document::CurrentPageForForm() const { return host_ ? host_->OnFormCurrentPage() : 0; }

bool Document::Save(const std::wstring& path, std::wstring& error) {
  if (form_) FORM_ForceToKillFocus(form_);
  const std::wstring dir = fw::DirOf(path);
  const std::wstring tmp = dir + L"\\~" + fw::FileNameOf(path) + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";

  auto writeTo = [&](FPDF_DWORD flags) -> bool {
    FileWriter w;
    w.version = 1;
    w.WriteBlock = WriteBlock;
    w.h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w.h == INVALID_HANDLE_VALUE) return false;
    bool ok = FPDF_SaveAsCopy(doc_, &w, flags) && w.ok;
    ok = ok && FlushFileBuffers(w.h);
    CloseHandle(w.h);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
  };

  if (!writeTo(FPDF_INCREMENTAL) && !writeTo(FPDF_NO_INCREMENTAL)) {
    DWORD e = GetLastError();
    error = (e == ERROR_ACCESS_DENIED || e == ERROR_WRITE_PROTECT)
                ? L"Featherweight can't save in this folder."
                : L"The file couldn't be saved. " + util::LastErrorMessage(e);
    return false;
  }

  bool replaced = false;
  const bool samePath = fw::EqualsNoCase(util::FullPath(path), util::FullPath(path_));
  if (util::FileExists(path)) {
    if (file_ == INVALID_HANDLE_VALUE || !samePath) {
      replaced = ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                              nullptr, nullptr) != FALSE;
    }
    if (!replaced) replaced = PosixReplace(tmp, path);
  } else {
    replaced = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
  }
  if (!replaced) {
    DWORD e = GetLastError();
    DeleteFileW(tmp.c_str());
    error = (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_USER_MAPPED_FILE)
                ? L"Another program is using the file, so it couldn't be replaced."
                : (e == ERROR_ACCESS_DENIED) ? L"Featherweight can't save in this folder."
                                             : L"The file couldn't be saved. " + util::LastErrorMessage(e);
    return false;
  }
  path_ = path;
  modified_ = false;
  return true;
}

// ---------------------------------------------------------------------------
// Highlights and notes

bool Document::AddHighlight(int index, const std::vector<FS_RECTF>& rects) {
  FPDF_PAGE page = Page(index);
  if (!page || rects.empty()) return false;
  FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_HIGHLIGHT);
  if (!annot) return false;
  FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 255, 236, 80, 255);  // highlighter yellow
  FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT);
  FPDFAnnot_SetRect(annot, &rects.front());
  for (const FS_RECTF& r : rects) {
    // PDF quad point order: top-left, top-right, bottom-left, bottom-right.
    FS_QUADPOINTSF q{r.left, r.top, r.right, r.top, r.left, r.bottom, r.right, r.bottom};
    FPDFAnnot_AppendAttachmentPoints(annot, &q);
  }
  FPDFPage_CloseAnnot(annot);
  modified_ = true;
  return true;
}

int Document::AddNote(int index, float x, float y, const std::wstring& text) {
  FPDF_PAGE page = Page(index);
  if (!page) return -1;
  FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_TEXT);
  if (!annot) return -1;
  FS_RECTF rect{x, y, x + 20, y - 20};
  FPDFAnnot_SetRect(annot, &rect);
  FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 255, 185, 0, 255);
  FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT | FPDF_ANNOT_FLAG_NOZOOM | FPDF_ANNOT_FLAG_NOROTATE);
  FPDFAnnot_SetStringValue(annot, "Contents", reinterpret_cast<FPDF_WIDESTRING>(text.c_str()));
  // Our own modern icon (a rounded amber speech bubble), stored in the file so other readers show it too.
  std::wstring ap = NoteAppearance(x, y);
  FPDFAnnot_SetAP(annot, FPDF_ANNOT_APPEARANCEMODE_NORMAL, reinterpret_cast<FPDF_WIDESTRING>(ap.c_str()));
  int result = FPDFPage_GetAnnotIndex(page, annot);
  FPDFPage_CloseAnnot(annot);
  modified_ = true;
  return result;
}

bool Document::SetNoteText(int index, int annotIndex, const std::wstring& text) {
  FPDF_PAGE page = Page(index);
  if (!page) return false;
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, annotIndex);
  if (!annot) return false;
  bool ok = FPDFAnnot_SetStringValue(annot, "Contents", reinterpret_cast<FPDF_WIDESTRING>(text.c_str())) != 0;
  FPDFPage_CloseAnnot(annot);
  modified_ = modified_ || ok;
  return ok;
}

std::wstring Document::NoteText(int index, int annotIndex) {
  FPDF_PAGE page = Page(index);
  if (!page) return {};
  FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, annotIndex);
  if (!annot) return {};
  std::wstring text;
  unsigned long bytes = FPDFAnnot_GetStringValue(annot, "Contents", nullptr, 0);
  if (bytes > 2) {
    text.resize(bytes / 2);
    FPDFAnnot_GetStringValue(annot, "Contents", reinterpret_cast<FPDF_WCHAR*>(text.data()), bytes);
    text.resize(wcsnlen(text.c_str(), text.size()));
  }
  FPDFPage_CloseAnnot(annot);
  return text;
}

AnnotHit Document::AnnotAt(int index, double x, double y) {
  AnnotHit hit;
  FPDF_PAGE page = Page(index);
  if (!page) return hit;
  auto inside = [&](const FS_RECTF& r) {
    return x >= std::min(r.left, r.right) && x <= std::max(r.left, r.right) && y >= std::min(r.top, r.bottom) &&
           y <= std::max(r.top, r.bottom);
  };
  // Topmost first.
  for (int i = FPDFPage_GetAnnotCount(page) - 1; i >= 0 && hit.kind == AnnotHit::Kind::None; --i) {
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
    if (!annot) continue;
    FPDF_ANNOTATION_SUBTYPE type = FPDFAnnot_GetSubtype(annot);
    FS_RECTF rect{};
    FPDFAnnot_GetRect(annot, &rect);
    if (type == FPDF_ANNOT_TEXT && inside(rect)) {
      hit = {AnnotHit::Kind::Note, i, rect};
    } else if (type == FPDF_ANNOT_HIGHLIGHT) {
      size_t quads = FPDFAnnot_CountAttachmentPoints(annot);
      for (size_t q = 0; q < quads; ++q) {
        FS_QUADPOINTSF p{};
        if (!FPDFAnnot_GetAttachmentPoints(annot, q, &p)) continue;
        FS_RECTF r{std::min({p.x1, p.x2, p.x3, p.x4}), std::max({p.y1, p.y2, p.y3, p.y4}),
                   std::max({p.x1, p.x2, p.x3, p.x4}), std::min({p.y1, p.y2, p.y3, p.y4})};
        if (inside(r)) {
          hit = {AnnotHit::Kind::Highlight, i, rect};
          break;
        }
      }
    }
    FPDFPage_CloseAnnot(annot);
  }
  return hit;
}

bool Document::RemoveAnnot(int index, int annotIndex) {
  FPDF_PAGE page = Page(index);
  if (!page) return false;
  bool ok = FPDFPage_RemoveAnnot(page, annotIndex) != 0;
  modified_ = modified_ || ok;
  return ok;
}

std::vector<NoteInfo> Document::NotesOnPage(int index) {
  std::vector<NoteInfo> notes;
  if (index < 0 || index >= PageCount()) return notes;
  // Use the loaded page if there is one; otherwise load it briefly without running page scripts.
  FPDF_PAGE page = pages_[index];
  const bool temporary = page == nullptr;
  if (temporary) page = FPDF_LoadPage(doc_, index);
  if (!page) return notes;
  const int count = FPDFPage_GetAnnotCount(page);
  for (int i = 0; i < count; ++i) {
    FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
    if (!annot) continue;
    if (FPDFAnnot_GetSubtype(annot) == FPDF_ANNOT_TEXT) {
      FS_RECTF r{};
      FPDFAnnot_GetRect(annot, &r);
      int dx = 0, dy = 0;
      FPDF_PageToDevice(page, 0, 0, 10000, 10000, 0, r.left, r.top, &dx, &dy);
      notes.push_back({index, i, dx / 10000.0f, dy / 10000.0f, r});
    }
    FPDFPage_CloseAnnot(annot);
  }
  if (temporary) FPDF_ClosePage(page);
  return notes;
}
