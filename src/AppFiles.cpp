// Keeping files safe, at the window's level (plan §6.3, §13; milestone D2 "Files"): new
// documents, offering what a crash left unsaved, noticing changes on disk, the autosave timer,
// and the backups in ⋯. What happens to one file is in word/DocFile.
#include <shlobj.h>

#include "App.h"
#include "Commands.h"
#include "Util.h"
#include "core/FileKinds.h"
#include "core/Strings.h"
#include "pkg/Backups.h"
#include "pkg/Recovery.h"
#include "word/DocView.h"

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

}  // namespace

void App::InitFiles(bool userFolders) {
  DocFile::Setup& s = DocFile::Config();
  s = {};
  wchar_t buf[LOCALE_NAME_MAX_LENGTH > 257 ? LOCALE_NAME_MAX_LENGTH : 257] = {};
  DWORD n = 257;
  if (GetUserNameW(buf, &n)) s.user = buf;
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

void App::NewDocument() {
  auto view = DocView::NewDocument(*this, ++newDocuments_);
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
          ? L"Featherweight closed before “" + orphans_.front().title + L"” was saved."
          : L"Featherweight closed before " + std::to_wstring(orphans_.size()) + L" documents were saved.";
  banner_.Show(BannerKind::Warning, text, {{kActRecover, L"Recover", true}, {kActDiscard, L"Discard"}});
}

void App::RecoverDocuments(bool open) {
  std::vector<recovery::Entry> entries = std::move(orphans_);
  orphans_.clear();
  int toActivate = -1;
  size_t failed = 0;
  for (const recovery::Entry& e : entries) {
    if (!open) {
      recovery::Remove(e.file);
      continue;
    }
    recovery::Entry entry;
    std::vector<uint8_t> bytes;
    if (!recovery::Read(e.file, entry, bytes)) {
      ++failed;
      continue;
    }
    // Saved with Save As, starting where the document was: the original is never replaced unasked.
    std::wstring name = entry.title;
    if (fw::KindOfPath(name) != fw::FileKind::Document) name += L".docx";
    const std::wstring suggested = entry.originalPath.empty() ? name : entry.originalPath;
    auto view = DocView::FromBytes(*this, std::move(bytes), Tagged(name, L"recovered"), suggested, true);
    view->SetViewport(DocRect(), ui_);
    std::wstring error;
    if (view->Load(L"", error) == OpenResult::Failed) {
      ++failed;
      continue;
    }
    view->ApplyState(FileState{});
    view->Autosave();  // into this session's folder, before the old autosave goes
    recovery::Remove(e.file);
    tabs_.push_back(std::move(view));
    toActivate = static_cast<int>(tabs_.size()) - 1;
  }
  if (toActivate >= 0) ActivateTab(toActivate);
  if (failed)
    banner_.Show(BannerKind::Error, failed == 1 ? L"A document couldn’t be recovered." :
                                                  std::to_wstring(failed) + L" documents couldn’t be recovered.",
                 {}, 8000);
  UpdateAutosaveTimer();
  Layout();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CheckActiveFile() {
  TabContent* v = Active();
  if (!v || !v->Ready()) return;
  switch (v->CheckFileOnDisk()) {
    case FileChange::None: return;
    case FileChange::Changed:
      banner_.Show(BannerKind::Warning,
                   v->Title() + L" was changed by another program." +
                       (v->Modified() ? L" Reloading it discards your changes here." : L""),
                   {{kActReload, L"Reload", true}, {kActNotNow, L"Keep this version"}});
      break;
    case FileChange::Missing:
      banner_.Show(BannerKind::Warning, v->Title() + L" was moved or deleted. Save it again to keep it.");
      break;
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
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
  if (search_.IsOpen()) CloseSearch();
  panel_.Show(nullptr, false);
  tabs_[index] = std::move(view);  // the banner said changes here are discarded
  active_ = -1;
  ActivateTab(index);
  UpdateAutosaveTimer();
  banner_.Show(BannerKind::Success, L"Reloaded", {}, 1600);
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
    banner_.Show(BannerKind::Error, L"The original couldn’t be found in the backups.", {}, 6000);
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
               n == 0 ? std::wstring(L"There are no backups.")
                      : n == 1 ? std::wstring(L"Deleted 1 backup") : L"Deleted " + std::to_wstring(n) + L" backups",
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
