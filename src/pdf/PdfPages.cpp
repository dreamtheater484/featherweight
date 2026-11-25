// The PDF tab's page tools (F1, f1-pdf): the Pages panel's actions, their Undo, and the files
// they read and write. The operations themselves are pdftools' (tools/PageOps), through PDFium.
#include <windows.h>

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>

#include "Commands.h"
#include "FileSession.h"
#include "Util.h"
#include "core/Strings.h"
#include "pdf/PagesPanel.h"
#include "pdf/PdfText.h"
#include "pdf/PdfView.h"
#include "pdf/tools/PageOps.h"

using Microsoft::WRL::ComPtr;

namespace {

// Undo keeps the document as it was before each change: at most this many, and this much in all.
constexpr size_t kUndoSteps = 30;
constexpr size_t kUndoBytes = size_t{256} << 20;

std::wstring BaseName(const std::wstring& path) {
  std::wstring name = fw::FileNameOf(path);
  if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos) name.resize(dot);
  return name;
}

// "report (pages 2-4)" or "report (page 3)".
std::wstring PartName(const std::wstring& base, const std::vector<int>& pages) {
  return Fmt(pages.size() == 1 ? T(L"{0} (page {1})") : T(L"{0} (pages {1})"), {base, pdftools::PageList(pages)});
}

// `path`, or "path (2).pdf" and so on when it's taken.
std::wstring FreePath(const std::wstring& path) {
  if (!util::FileExists(path)) return path;
  const std::wstring stem = path.size() > 4 ? path.substr(0, path.size() - 4) : path;
  for (int k = 2; k < 1000; ++k) {
    const std::wstring next = stem + L" (" + std::to_wstring(k) + L").pdf";
    if (!util::FileExists(next)) return next;
  }
  return path;
}

// Writes a new PDF the way saves are written: safely, and keeping a file it replaces in the backups.
bool WritePdf(const std::wstring& path, const std::string& bytes, std::wstring& error) {
  FileSession::SaveJob job;
  job.target = path;
  job.name = fw::FileNameOf(path);
  job.backups = FileSession::Config().backups;
  job.keepExisting = util::FileExists(path);
  const fw::pkg::SaveResult r = FileSession::WriteSave(job, std::vector<uint8_t>(bytes.begin(), bytes.end()), nullptr);
  if (!r.Ok()) error = fw::Utf8ToWide(r.message);
  return r.Ok();
}

}  // namespace

std::wstring PdfView::PageToolsBlocked() const {
  if (!doc_ || !Ready()) return T(L"The page tools work once the PDF is open.");
  switch (pdftools::PageToolsBlock(doc_->Handle(), doc_->IsXfa())) {
    case pdftools::Block::Xfa:
      return T(L"This PDF is an XFA form, whose pages come from the form, so the page tools are off.");
    case pdftools::Block::Protected:
      return T(L"This PDF is protected against changes, so the page tools are off.");
    case pdftools::Block::None: break;
  }
  return {};
}

std::vector<int> PdfView::ToolPages() const {
  if (pagesPanel_ && host_.ViewShownPanel() == pagesPanel_.get()) {
    std::vector<int> sel = pagesPanel_->Selection();
    if (!sel.empty()) return sel;
  }
  if (!Ready() || PageCount() == 0) return {};
  return {std::clamp(CurrentPage(), 0, PageCount() - 1)};
}

ID2D1Bitmap* PdfView::Thumbnail(int page, int priority) {
  if (!Ready() || !cache_ || page < 0 || page >= PageCount()) return nullptr;
  ID2D1Bitmap* bmp = cache_->Preview(page, rotation_, nullptr);
  // A preview is at most RenderCache::kPreviewMax pixels long; asking at twice the points gets that.
  fw::PageSizePt s = doc_->PageSizes()[page];
  if (rotation_ % 2) std::swap(s.w, s.h);
  const double k = 640.0 / std::max(1.0, static_cast<double>(std::max(s.w, s.h)));
  cache_->WantPreview(page, rotation_,
                      {std::max(1, static_cast<int>(s.w * k)), std::max(1, static_cast<int>(s.h * k))}, priority);
  return bmp;
}

void PdfView::TogglePagesPanel() {
  if (!Ready()) return;
  if (pagesPanel_ && host_.ViewShownPanel() == pagesPanel_.get()) {
    host_.ViewShowPanel(nullptr);
    return;
  }
  if (!pagesPanel_) pagesPanel_ = std::make_unique<PagesPanel>(*this);
  pagesPanel_->Clamp();
  const int page = std::clamp(CurrentPage(), 0, std::max(0, PageCount() - 1));
  host_.ViewShowPanel(pagesPanel_.get());
  pagesPanel_->Select({page}, page);
}

void PdfView::ForgetPages() {
  if (cache_) cache_->Clear();
  selection_.Clear();
  selRects_.clear();
  selRectsVersion_ = -1;
  selBar_ = false;
  search_.Clear();
  lastSearchCurrent_ = -1;
  hoverNote_ = HoverNote{};
  pendingNote_ = NoteTarget{};
  contextAnnot_ = AnnotHit{};
  contextPage_ = formPage_ = formHoverPage_ = markerHover_ = -1;
  caret_.page = -1;
  if (doc_) doc_->CloseAllPages();
}

bool PdfView::BeginPageChange() {
  if (!Ready()) return false;
  if (ocr_) {
    host_.ViewMessage(BannerKind::Info, T(L"Text is being recognized. Wait for it, or stop it with Esc."));
    return false;
  }
  if (const std::wstring why = PageToolsBlocked(); !why.empty()) {
    host_.ViewMessage(BannerKind::Info, why);
    return false;
  }
  if (doc_->SignatureCount() > 0 && !signatureWarned_) {
    if (!host_.ViewConfirm(T(L"This PDF is signed. Changing it breaks the signature: readers will show it was changed "
                             L"after it was signed."),
                           T(L"Change it")))
      return false;
    signatureWarned_ = true;
  }
  pageBeforeModified_ = doc_->Modified();
  if (!doc_->CurrentBytes(pageBefore_) || pageBefore_.size() > kUndoBytes / 2) pageBefore_.clear();
  ForgetPages();
  return true;
}

void PdfView::EndPageChange(bool ok, int focusPage) {
  if (ok) {
    if (!pageBefore_.empty()) {
      pageUndo_.push_back({std::move(pageBefore_), pageBeforeModified_});
      size_t total = 0;
      for (const PageUndo& u : pageUndo_) total += u.bytes.size();
      while (pageUndo_.size() > kUndoSteps || (total > kUndoBytes && pageUndo_.size() > 1)) {
        total -= pageUndo_.front().bytes.size();
        pageUndo_.erase(pageUndo_.begin());
      }
    }
    pageRedo_.clear();
    doc_->PagesChanged();
  } else if (!pageBefore_.empty()) {
    doc_->Reload(std::move(pageBefore_), pageBeforeModified_);  // a change that failed halfway goes back
  } else {
    doc_->RefreshPageSizes();
  }
  pageBefore_.clear();
  const int count = PageCount();
  notesScanned_.assign(count, false);
  notes_.clear();
  noteScanNext_ = 0;
  if (pagesPanel_) pagesPanel_->Clamp();
  Relayout(nullptr);
  if (focusPage >= 0 && count > 0) GoToPage(std::min(focusPage, count - 1));
  lastModified_ = doc_->Modified();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

bool PdfView::RotatePages(const std::vector<int>& pages, int quarterTurns) {
  if (pages.empty() || !BeginPageChange()) return false;
  const int keep = CurrentPage();
  const bool ok = pdftools::Rotate(doc_->Handle(), pages, quarterTurns);
  EndPageChange(ok, keep);
  if (ok && pagesPanel_) pagesPanel_->Select(pages, pages.front());
  return ok;
}

bool PdfView::DeletePages(const std::vector<int>& pages) {
  if (!Ready() || pages.empty()) return false;
  if (static_cast<int>(pages.size()) >= PageCount()) {
    host_.ViewMessage(BannerKind::Info, T(L"A PDF needs at least one page, so not every page can be deleted."));
    return false;
  }
  if (!BeginPageChange()) return false;
  const bool undoable = !pageBefore_.empty();
  if (!undoable && !host_.ViewConfirm(Fmt(T(L"Delete {0}? This PDF is too large to undo it."),
                                                    {pdftext::Pages(static_cast<int>(pages.size()))}),
                                      T(L"Delete"))) {
    EndPageChange(false, CurrentPage());
    return false;
  }
  const int first = *std::min_element(pages.begin(), pages.end());
  const bool ok = pdftools::Delete(doc_->Handle(), pages);
  EndPageChange(ok, first);
  if (!ok) return false;
  const int at = std::min(first, PageCount() - 1);
  if (pagesPanel_) pagesPanel_->Select({at}, at);
  host_.ViewMessage(BannerKind::Success,
                    undoable ? Fmt(T(L"Deleted {0}. Ctrl+Z brings them back."),
                                             {pdftext::Pages(static_cast<int>(pages.size()))})
                             : Fmt(T(L"Deleted {0}."), {pdftext::Pages(static_cast<int>(pages.size()))}));
  return true;
}

bool PdfView::MovePages(const std::vector<int>& pages, int gap) {
  if (pages.empty()) return false;
  std::vector<int> sorted = pages;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  const int dest = pdftools::MoveDestination(sorted, gap);
  bool same = true;
  for (size_t k = 0; k < sorted.size(); ++k) same = same && sorted[k] == dest + static_cast<int>(k);
  if (same) return false;  // dropped where they are
  if (!BeginPageChange()) return false;
  // What the pages inherit from the page tree goes on the pages first: a move to another branch
  // would give them that branch's (f1-gate's finding).
  std::string bytes = pageBefore_, pinned;
  if ((!bytes.empty() || doc_->CurrentBytes(bytes)) && pdftools::PinInherited(bytes, sorted, PageCount(), pinned))
    doc_->Reload(std::move(pinned), true);
  const bool ok = pdftools::Move(doc_->Handle(), sorted, gap);
  EndPageChange(ok, dest);
  if (ok && pagesPanel_) {
    std::vector<int> moved;
    for (size_t k = 0; k < sorted.size(); ++k) moved.push_back(dest + static_cast<int>(k));
    pagesPanel_->Select(moved, dest);
  }
  return ok;
}

bool PdfView::InsertPagesFromFiles(int at) {
  if (!Ready()) return false;
  if (const std::wstring why = PageToolsBlocked(); !why.empty()) {
    host_.ViewMessage(BannerKind::Info, why);
    return false;
  }
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  const std::wstring pdfName = T(L"PDF documents");
  const COMDLG_FILTERSPEC types[] = {{pdfName.c_str(), L"*.pdf"}};
  dlg->SetFileTypes(1, types);
  const std::wstring title = T(L"Insert pages from a PDF");
  dlg->SetTitle(title.c_str());
  const std::wstring ok = T(L"Insert");
  dlg->SetOkButtonLabel(ok.c_str());
  DWORD options = 0;
  dlg->GetOptions(&options);
  dlg->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
  if (!Path().empty()) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(fw::DirOf(Path()).c_str(), nullptr, IID_PPV_ARGS(&folder))))
      dlg->SetFolder(folder.Get());
  }
  if (FAILED(dlg->Show(host_.ViewHwnd()))) return false;
  ComPtr<IShellItemArray> items;
  DWORD n = 0;
  if (FAILED(dlg->GetResults(&items)) || FAILED(items->GetCount(&n))) return false;
  std::vector<std::wstring> paths;
  for (DWORD i = 0; i < n; ++i) {
    ComPtr<IShellItem> item;
    PWSTR file = nullptr;
    if (FAILED(items->GetItemAt(i, &item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &file))) continue;
    paths.push_back(file);
    CoTaskMemFree(file);
  }
  return InsertFiles(paths, at);
}

bool PdfView::InsertFiles(const std::vector<std::wstring>& paths, int at) {
  if (!Ready()) return false;
  // Every file read first: nothing changes unless at least one can be inserted.
  std::vector<std::unique_ptr<pdftools::Source>> sources;
  std::vector<std::wstring> names;
  std::wstring problem;
  for (const std::wstring& path : paths) {
    const std::wstring name = L"“" + fw::FileNameOf(path) + L"”";
    std::string bytes;
    auto src = std::make_unique<pdftools::Source>();
    const pdftools::SourceError err = util::ReadFileBytes(path, bytes, size_t{1} << 30)
                                          ? pdftools::OpenSource(std::move(bytes), {}, *src)
                                          : pdftools::SourceError::Read;
    switch (err) {
      case pdftools::SourceError::None:
        sources.push_back(std::move(src));
        names.push_back(name);
        continue;
      case pdftools::SourceError::Read: problem = Fmt(T(L"{0} couldn't be read."), {name}); break;
      case pdftools::SourceError::NotPdf: problem = Fmt(T(L"{0} isn't a PDF, or it's damaged."), {name}); break;
      case pdftools::SourceError::Password:
        problem = Fmt(T(L"{0} has a password. Open it, save a copy without one, and insert that."), {name});
        break;
      case pdftools::SourceError::NotAllowed:
        problem = Fmt(T(L"{0} doesn't allow copying its pages."), {name});
        break;
      case pdftools::SourceError::Empty: problem = Fmt(T(L"{0} has no pages."), {name}); break;
    }
  }
  if (sources.empty()) {
    if (!problem.empty()) host_.ViewMessage(BannerKind::Error, problem);
    return false;
  }
  if (!BeginPageChange()) return false;
  at = std::clamp(at, 0, PageCount());
  const int first = at;
  int added = 0;
  bool okAll = true;
  for (const auto& src : sources) {
    const int got = pdftools::Insert(doc_->Handle(), *src, at);
    okAll = okAll && got > 0;
    at += got;
    added += got;
  }
  // The inserted pages read from the sources until the document is written and opened again
  // (f1-gate's finding: closing them first crashed a later save). If that can't be done, the
  // insert goes back; with no snapshot to go back to, the sources stay open with the document.
  if (added > 0 && !doc_->ReloadAsEdited()) {
    if (!pageBefore_.empty()) {
      EndPageChange(false, first);
      host_.ViewMessage(BannerKind::Error, T(L"The pages couldn't be inserted."));
      return false;
    }
    for (auto& src : sources) importedFrom_.push_back(std::move(src));
  }
  sources.clear();
  EndPageChange(added > 0, first);
  if (added == 0) {
    host_.ViewMessage(BannerKind::Error, T(L"The pages couldn't be inserted."));
    return false;
  }
  if (pagesPanel_) {
    std::vector<int> inserted;
    for (int p = first; p < first + added; ++p) inserted.push_back(p);
    pagesPanel_->Select(inserted, first);
  }
  std::wstring message = names.size() == 1
                             ? Fmt(T(L"Inserted {0} from {1}."), {pdftext::Pages(added), names.front()})
                             : Fmt(T(L"Inserted {0} from {1} files."),
                                             {pdftext::Pages(added), std::to_wstring(names.size())});
  if (!problem.empty()) message += L" " + problem;
  host_.ViewMessage(problem.empty() && okAll ? BannerKind::Success : BannerKind::Warning, message);
  return true;
}

bool PdfView::ChoosePdfToSave(const std::wstring& suggested, const std::wstring& title, std::wstring& path,
                              bool allowThisFile) {
  ComPtr<IFileSaveDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  const std::wstring pdfName = T(L"PDF document");
  const COMDLG_FILTERSPEC types[] = {{pdfName.c_str(), L"*.pdf"}};
  dlg->SetFileTypes(1, types);
  dlg->SetDefaultExtension(L"pdf");
  dlg->SetFileName(suggested.c_str());
  dlg->SetTitle(title.c_str());
  if (!Path().empty()) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(fw::DirOf(Path()).c_str(), nullptr, IID_PPV_ARGS(&folder))))
      dlg->SetFolder(folder.Get());
  }
  if (FAILED(dlg->Show(host_.ViewHwnd()))) return false;
  ComPtr<IShellItem> item;
  PWSTR file = nullptr;
  if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &file))) return false;
  path = file;
  CoTaskMemFree(file);
  if (!allowThisFile && fw::EqualsNoCase(util::FullPath(path), util::FullPath(Path()))) {
    host_.ViewMessage(BannerKind::Info, T(L"That's this PDF. Choose another name for the new file."));
    return false;
  }
  return true;
}

bool PdfView::ExtractPages(const std::vector<int>& pages) {
  if (!Ready() || pages.empty()) return false;
  if (const std::wstring why = PageToolsBlocked(); !why.empty()) {
    host_.ViewMessage(BannerKind::Info, why);
    return false;
  }
  std::vector<int> sorted = pages;
  std::sort(sorted.begin(), sorted.end());
  std::wstring path;
  if (!ChoosePdfToSave(PartName(BaseName(Path()), sorted), T(L"Extract pages as a new PDF"), path)) return false;
  return ExtractTo(sorted, path);
}

bool PdfView::ExtractTo(const std::vector<int>& sorted, const std::wstring& path) {
  if (!Ready() || sorted.empty()) return false;
  std::string bytes;
  std::wstring error;
  if (!pdftools::Extract(doc_->Handle(), sorted, bytes)) error = T(L"The pages couldn't be extracted.");
  else WritePdf(path, bytes, error);
  if (!error.empty()) {
    host_.ViewMessage(BannerKind::Error, error);
    return false;
  }
  host_.ViewMessage(BannerKind::Success, Fmt(T(L"Extracted {0} to {1}."),
                                                       {pdftext::Pages(static_cast<int>(sorted.size())), fw::FileNameOf(path)}));
  return true;
}

bool PdfView::SplitPages(const std::vector<std::vector<int>>& parts) {
  if (!Ready() || parts.size() < 2) return false;
  if (const std::wstring why = PageToolsBlocked(); !why.empty()) {
    host_.ViewMessage(BannerKind::Info, why);
    return false;
  }
  std::wstring path;
  if (!ChoosePdfToSave(BaseName(Path()), T(L"Split: where the new PDFs go, and their name"), path)) return false;
  return SplitTo(parts, path);
}

bool PdfView::SplitTo(const std::vector<std::vector<int>>& parts, const std::wstring& path) {
  if (!Ready() || parts.size() < 2) return false;
  const std::wstring dir = fw::DirOf(path), base = BaseName(path);
  int written = 0;
  std::wstring error;
  for (const std::vector<int>& part : parts) {
    std::string bytes;
    if (!pdftools::Extract(doc_->Handle(), part, bytes)) {
      error = T(L"A part couldn't be made.");
      break;
    }
    if (!WritePdf(FreePath(dir + L"\\" + PartName(base, part) + L".pdf"), bytes, error)) break;
    ++written;
  }
  if (!error.empty()) {
    host_.ViewMessage(BannerKind::Error,
                      written > 0 ? Fmt(T(L"{0} files were made, then: {1}"), {std::to_wstring(written), error})
                                  : error);
    return false;
  }
  host_.ViewMessage(BannerKind::Success,
                    Fmt(T(L"Split into {0} PDFs in {1}."), {std::to_wstring(written), fw::FileNameOf(dir)}));
  return true;
}

bool PdfView::UndoPages() {
  if (pageUndo_.empty() || !Ready()) return false;
  PageUndo now;
  now.modified = doc_->Modified();
  if (!doc_->CurrentBytes(now.bytes)) return false;
  ForgetPages();
  PageUndo back = std::move(pageUndo_.back());
  pageUndo_.pop_back();
  if (!doc_->Reload(std::move(back.bytes), back.modified)) {
    EndPageChange(false, CurrentPage());
    return false;
  }
  pageRedo_.push_back(std::move(now));
  pageBefore_.clear();
  EndPageChange(false, CurrentPage());
  return true;
}

bool PdfView::RedoPages() {
  if (pageRedo_.empty() || !Ready()) return false;
  PageUndo now;
  now.modified = doc_->Modified();
  if (!doc_->CurrentBytes(now.bytes)) return false;
  ForgetPages();
  PageUndo again = std::move(pageRedo_.back());
  pageRedo_.pop_back();
  if (!doc_->Reload(std::move(again.bytes), again.modified)) {
    EndPageChange(false, CurrentPage());
    return false;
  }
  pageUndo_.push_back(std::move(now));
  pageBefore_.clear();
  EndPageChange(false, CurrentPage());
  return true;
}

// Scripted runs (--script pdftool:...): the page tools without their dialogs, for the screen tests.
//   select:1,3-4   rotate:1 | rotate:-1   delete   move:GAP (1-based: before page GAP)
//   insert:PATH|AT (AT 1-based, 0 or empty: at the end)   extract:PATH   split:EVERY|PATH   undo   redo
bool PdfView::RunPageScript(const std::wstring& arg) {
  if (!Ready()) return false;
  const size_t colon = arg.find(L':');
  const std::wstring op = arg.substr(0, colon), rest = colon == std::wstring::npos ? L"" : arg.substr(colon + 1);
  auto field = [&](int k) {
    size_t from = 0;
    for (int i = 0; i < k && from != std::wstring::npos; ++i) {
      from = rest.find(L'|', from);
      if (from != std::wstring::npos) ++from;
    }
    if (from == std::wstring::npos) return std::wstring();
    const size_t to = rest.find(L'|', from);
    return rest.substr(from, to == std::wstring::npos ? std::wstring::npos : to - from);
  };
  if (op == L"select") {
    if (!pagesPanel_ || host_.ViewShownPanel() != pagesPanel_.get()) TogglePagesPanel();
    std::vector<int> pages;
    for (size_t at = 0; at < rest.size();) {
      size_t end = rest.find(L',', at);
      if (end == std::wstring::npos) end = rest.size();
      const std::wstring item = rest.substr(at, end - at);
      const size_t dash = item.find(L'-');
      const int a = _wtoi(item.c_str()), b = dash == std::wstring::npos ? a : _wtoi(item.c_str() + dash + 1);
      for (int p = a; p <= b; ++p) pages.push_back(p - 1);
      at = end + 1;
    }
    pagesPanel_->Select(pages, pages.empty() ? -1 : pages.front());
    return true;
  }
  if (op == L"rotate") return RotatePages(ToolPages(), rest.empty() ? 1 : _wtoi(rest.c_str()));
  if (op == L"delete") return DeletePages(ToolPages());
  if (op == L"move") return MovePages(ToolPages(), _wtoi(rest.c_str()) - 1);
  if (op == L"insert") {
    const int at = _wtoi(field(1).c_str());
    return InsertFiles({field(0)}, at > 0 ? at - 1 : PageCount());
  }
  if (op == L"extract") return ExtractTo(ToolPages(), rest);
  if (op == L"split") return SplitTo(pdftools::SplitEvery(PageCount(), std::max(1, _wtoi(field(0).c_str()))), field(1));
  if (op == L"ocr") return OcrNow(rest.empty() ? std::vector<int>{} : ToolPages()) > 0;
  if (op == L"sign") return SignWithPfxForTesting(field(0), field(1), field(2));
  if (op == L"checksig") {
    CheckSignatures(true);
    return doc_->SignatureCount() > 0;
  }
  if (op == L"stamp") return StampForTesting();
  if (op == L"undo") return UndoPages();
  if (op == L"redo") return RedoPages();
  return false;
}
