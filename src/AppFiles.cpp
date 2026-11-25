// Keeping files safe, at the window's level (plan §6.3, §13; milestone D2 "Files"): new
// documents, offering what a crash left unsaved, noticing changes on disk, the autosave timer,
// and the backups in ⋯. What happens to one file is in word/DocFile.
#include <shlobj.h>

#include "App.h"
#include "Commands.h"
#include "Util.h"
#include "core/FileKinds.h"
#include "core/Lang.h"
#include "core/Strings.h"
#include "pkg/Backups.h"
#include "pkg/Recovery.h"
#include "sheet/view/WorkbookFile.h"
#include "sheet/view/WorkbookView.h"
#include "slide/view/PresentationView.h"
#include "word/DocView.h"

#include <algorithm>
#include <string_view>

namespace recovery = fw::pkg::recovery;
namespace backups = fw::pkg::backups;

namespace {

constexpr UINT kAutosaveMs = 30000;

// "Plan.docx" -> "Plan (recovered).docx"
std::wstring Tagged(const std::wstring& name, const wchar_t* tag) {
  const size_t dot = name.rfind(L'.');
  if (dot == std::wstring::npos || dot == 0) return name + L" (" + tag + L")";
  return name.substr(0, dot) + L" (" + tag + L")" + name.substr(dot);
}

// An autosave of a workbook never saved ("Book1") has no extension to tell: its package does.
bool WorkbookPackage(const std::vector<uint8_t>& bytes) {
  constexpr std::string_view kPart = "xl/workbook.";
  return std::search(bytes.begin(), bytes.end(), kPart.begin(), kPart.end()) != bytes.end();
}

}  // namespace

void App::InitFiles(bool userFolders) {
  DocFile::Setup& s = DocFile::Config();
  s = {};
  wchar_t buf[LOCALE_NAME_MAX_LENGTH > 257 ? LOCALE_NAME_MAX_LENGTH : 257] = {};
  DWORD n = 257;
  if (userFolders && GetUserNameW(buf, &n)) s.user = buf;  // no owner files from snapshots either
  if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH)) s.language = buf;
  DWORD paper = 0;
  if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IPAPERSIZE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&paper),
                      sizeof(paper) / sizeof(wchar_t)))
    s.a4 = paper == 9;  // DMPAPER_A4; Letter otherwise
  if (!userFolders) return;  // snapshots and the UI checks never write there

  // Portable copies keep their folders next to the exe, like their settings (§13).
  std::wstring base;
  if (!settings_.IsInstalled() && fw::EqualsNoCase(fw::DirOf(settings_.FilePath()), util::ExeDir())) {
    base = util::ExeDir();
  } else {
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
      base = std::wstring(local) + L"\\Featherweight";
      CoTaskMemFree(local);
    }
  }
  if (base.empty()) return;
  s.backups = base + L"\\Backups";
  s.recovery = base + L"\\Recovery";
}

void App::NewDocument(int kind) {
  auto view = DocView::NewDocument(*this, ++newDocuments_, static_cast<fw::doc::docx::Template>(kind));
  view->SetViewport(DocRect(), ui_);
  std::wstring error;
  if (view->Load(L"", error) == OpenResult::Failed) {
    banner_.Show(BannerKind::Error, error, {}, 8000);
    return;
  }
  view->ApplyState(FileState{});
  tabs_.push_back(std::move(view));
  ActivateTab(static_cast<int>(tabs_.size()) - 1);
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::NewWorkbook() {
  auto view = fw::sheet::view::WorkbookView::NewWorkbook(*this, ++newWorkbooks_);
  view->SetViewport(DocRect(), ui_);
  std::wstring error;
  if (view->Load(L"", error) == OpenResult::Failed) {
    banner_.Show(BannerKind::Error, error, {}, 8000);
    return;
  }
  view->ApplyState(FileState{});
  tabs_.push_back(std::move(view));
  ActivateTab(static_cast<int>(tabs_.size()) - 1);
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::NewPresentation() {
  // A blank 16:9 deck on the Office theme, as PowerPoint makes one (P2, p2-slides).
  auto view = fw::slide::view::PresentationView::NewPresentation(*this, ++newPresentations_);
  view->SetViewport(DocRect(), ui_);
  std::wstring error;
  if (view->Load(L"", error) == OpenResult::Failed) {
    banner_.Show(BannerKind::Error, error, {}, 8000);
    return;
  }
  view->ApplyState(FileState{});
  tabs_.push_back(std::move(view));
  ActivateTab(static_cast<int>(tabs_.size()) - 1);
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OfferRecovery() {
  orphans_ = recovery::FindOrphans(DocFile::Config().recovery);
  if (orphans_.empty()) return;
  const std::wstring text =
      orphans_.size() == 1
          ? Fmt(T(L"Featherweight closed before “{0}” was saved."), {orphans_.front().title})
          : Fmt(T(L"Featherweight closed before {0} documents were saved."), {std::to_wstring(orphans_.size())});
  banner_.Show(BannerKind::Warning, text, {{kActRecover, T(L"Recover"), true}, {kActDiscard, T(L"Discard")}});
}

void App::RecoverDocuments(bool open) {
  std::vector<recovery::Entry> entries = std::move(orphans_);
  orphans_.clear();
  int toActivate = -1;
  size_t failed = 0, partial = 0;
  for (const recovery::Entry& e : entries) {
    if (!open) {
      recovery::Remove(e.file);
      continue;
    }
    recovery::Entry entry;
    std::vector<uint8_t> bytes, records;
    // A journal (a workbook's): the document as it was opened, and its edits, made again below.
    if (!(e.journal ? recovery::ReadJournal(e.file, entry, bytes, records) : recovery::Read(e.file, entry, bytes))) {
      ++failed;
      continue;
    }
    // Saved with Save As, starting where the document was: the original is never replaced unasked.
    std::wstring name = entry.title, suggested = entry.originalPath;
    // A CSV file's autosave that holds a workbook (a chart, a table...): an unsaved .xlsx named after it.
    WorkbookFile::RecoveredAsWorkbook(bytes, name, suggested);
    const fw::FileKind kind = fw::KindOfPath(name);
    // A presentation's journal says so (PresentationFile: "pptx"); its charts hold workbooks.
    const bool presentation = kind == fw::FileKind::Presentation || (entry.journal && entry.meta == "pptx");
    const bool workbook = !presentation && (kind == fw::FileKind::Workbook || (kind != fw::FileKind::Document && WorkbookPackage(bytes)));
    if (presentation && kind != fw::FileKind::Presentation) name += L".pptx";
    if (workbook && kind != fw::FileKind::Workbook) name += L".xlsx";
    if (!workbook && !presentation && kind != fw::FileKind::Document) name += L".docx";
    if (suggested.empty()) {  // never saved: the Documents folder, as for a new document
      PWSTR docs = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        suggested = std::wstring(docs) + L"\\";
        CoTaskMemFree(docs);
      }
      suggested += name;
    }
    std::unique_ptr<TabContent> view;
    if (presentation)
      view = fw::slide::view::PresentationView::FromBytes(*this, std::move(bytes), Tagged(name, L"recovered"), suggested, true);
    else if (workbook)
      view = fw::sheet::view::WorkbookView::FromBytes(*this, std::move(bytes), Tagged(name, L"recovered"), suggested, true);
    else
      view = DocView::FromBytes(*this, std::move(bytes), Tagged(name, L"recovered"), suggested, true);
    view->SetViewport(DocRect(), ui_);
    std::wstring error;
    if (view->Load(L"", error) == OpenResult::Failed) {
      ++failed;
      continue;
    }
    if (!records.empty() &&
        !view->ReplayEdits(std::string_view(reinterpret_cast<const char*>(records.data()), records.size()), error))
      ++partial;  // the edits before the one that couldn't be made are back
    view->ApplyState(FileState{});
    view->Autosave();  // into this session's folder, before the old autosave goes
    recovery::Remove(e.file);
    tabs_.push_back(std::move(view));
    toActivate = static_cast<int>(tabs_.size()) - 1;
  }
  if (toActivate >= 0) ActivateTab(toActivate);
  if (failed)
    banner_.Show(BannerKind::Error,
                 failed == 1 ? std::wstring(T(L"A document couldn’t be recovered."))
                             : Fmt(T(L"{0} documents couldn’t be recovered."), {std::to_wstring(failed)}),
                 {}, 8000);
  else if (partial)
    banner_.Show(BannerKind::Warning, T(L"The last changes to a recovered file couldn’t all be made again. Check it before you save."),
                 {}, 8000);
  UpdateAutosaveTimer();
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CheckActiveFile() {
  TabContent* v = Active();
  if (!v || !v->Ready()) return;
  const FileChange change = v->CheckFileOnDisk();
  if (change == FileChange::None) return;
  ShowFileNotice(v, change);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowFileNotice(TabContent* v, FileChange change) {
  // The tab owns the notice: ActivateTab and CloseTab put it away and bring it back.
  fileNotice_ = change;
  if (change == FileChange::Changed)
    banner_.Show(BannerKind::Warning,
                 Fmt(T(L"{0} was changed by another program."), {v->Title()}) +
                     (v->Modified() ? T(L" Reloading it discards your changes here.") : L""),
                 {{kActReload, T(L"Reload"), true}, {kActNotNow, T(L"Keep this version")}}, 0, v);
  else if (change == FileChange::Missing)
    banner_.Show(BannerKind::Warning, Fmt(T(L"{0} was moved or deleted. Save it again to keep it."), {v->Title()}), {},
                 0, v);
}

void App::ReloadActive() {
  TabContent* v = Active();
  if (!v || v->Path().empty()) return;
  const int index = active_;
  auto view = CreateTab(v->Path());
  view->SetViewport(DocRect(), ui_);
  std::wstring error;
  if (view->Load(L"", error) == OpenResult::Failed) {
    banner_.Show(BannerKind::Error, error, {}, 8000);
    return;
  }
  view->ApplyState(v->State());
  std::erase_if(fileNotices_, [v](const auto& n) { return n.first == v; });  // the old view goes
  if (search_.IsOpen()) CloseSearch();
  panel_.Show(nullptr, false);
  toolbar_.Forget();  // the old view's own toolbar controls go with it
  ribbon_.Forget();
  tabs_[index] = std::move(view);  // the banner said changes here are discarded
  active_ = -1;
  ActivateTab(index);
  UpdateAutosaveTimer();
  banner_.Show(BannerKind::Success, T(L"Reloaded"), {}, 1600);
}

std::wstring App::LatestBackup() const {
  TabContent* v = Active();
  if (!v || v->Kind() != TabKind::Document || v->Path().empty()) return {};
  return backups::Latest(DocFile::Config().backups, v->Path());
}

void App::RestoreOriginal() {
  const std::wstring backup = LatestBackup();
  std::string data;
  if (backup.empty() || !util::ReadFileBytes(backup, data, 1u << 30)) {
    banner_.Show(BannerKind::Error, T(L"The original couldn’t be found in the backups."), {}, 6000);
    return;
  }
  // Opened as a copy beside the file, so nothing is replaced until you choose to.
  const std::wstring name = Tagged(backups::OriginalName(backup), L"original");
  auto view = DocView::FromBytes(*this, std::vector<uint8_t>(data.begin(), data.end()), name,
                                 fw::DirOf(Active()->Path()) + L"\\" + name, false);
  view->SetViewport(DocRect(), ui_);
  std::wstring error;
  if (view->Load(L"", error) == OpenResult::Failed) {
    banner_.Show(BannerKind::Error, error, {}, 8000);
    return;
  }
  view->ApplyState(FileState{});
  tabs_.push_back(std::move(view));
  ActivateTab(static_cast<int>(tabs_.size()) - 1);
}

void App::DeleteBackups() {
  const size_t n = backups::DeleteAll(DocFile::Config().backups);
  banner_.Show(BannerKind::Success,
               n == 0   ? std::wstring(T(L"There are no backups."))
               : n == 1 ? std::wstring(T(L"Deleted 1 backup"))
                        : Fmt(T(L"Deleted {0} backups"), {std::to_wstring(n)}),
               {}, 2400);
}

void App::UpdateAutosaveTimer() {
  if (!hwnd_) return;
  bool any = false;
  for (const auto& t : tabs_) any = any || t->Modified();
  if (any && !autosaveTimer_) SetTimer(hwnd_, kTimerAutosave, kAutosaveMs, nullptr);
  else if (!any && autosaveTimer_) KillTimer(hwnd_, kTimerAutosave);
  autosaveTimer_ = any;
}

void App::AutosaveAll() {
  for (const auto& t : tabs_)
    if (t->Modified()) t->Autosave();
}
