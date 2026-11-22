// The presentation tab's file (P2, p2-model; plan §4.3, §13): Ctrl+S and Save As write the deck on a
// worker thread through PresentationFile (only the changed parts are written, the rest copied as they
// are; the original kept in the backups on the first save), the recovery journal and its autosave,
// a recovered deck's edits made again, and the file changing on disk.

#include <windows.h>

#include "core/Strings.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Journal.h"
#include "slide/view/NotesEdit.h"
#include "slide/view/PresentationFile.h"
#include "slide/view/PresentationView.h"

namespace fw::slide::view {

namespace {

const SaveFormat kFormats[] = {
    {L"PowerPoint presentation", L"*.pptx", L"pptx"},
    {L"PowerPoint macro-enabled presentation", L"*.pptm", L"pptm"},
    {L"PowerPoint template", L"*.potx", L"potx"},
    {L"PowerPoint macro-enabled template", L"*.potm", L"potm"},
    {L"PowerPoint slide show", L"*.ppsx", L"ppsx"},
    {L"PowerPoint macro-enabled slide show", L"*.ppsm", L"ppsm"},
};

}  // namespace

std::unique_ptr<PresentationView> PresentationView::FromBytes(ViewHost& host, std::vector<uint8_t> bytes,
                                                              std::wstring title, std::wstring suggested,
                                                              bool unsaved) {
  auto view = std::make_unique<PresentationView>(host, std::wstring());
  view->untitled_ = title;
  view->bytes_ = std::move(bytes);
  view->fromBytes_ = true;
  view->File().Copy(std::move(title), std::move(suggested), unsaved);
  return view;
}

PresentationFile& PresentationView::File() {
  if (!file_) file_ = std::make_unique<PresentationFile>();
  return *file_;
}

bool PresentationView::Modified() const { return file_ && file_->Dirty(); }

void PresentationView::FileOpened() {
  PresentationFile& f = File();
  if (!fromBytes_ && !newDeck_) {
    if (Presentation* deck = deck_->Model()) f.Opened(path_, deck->GetPackage().GetSnapshot().GetStamp());
    if (std::wstring inUse = f.InUseMessage(); !inUse.empty()) host_.ViewMessage(BannerKind::Warning, inUse);
  }
  // Load then makes the deck's Editor (SlideEditor), and OnEditorMade attaches the file to it before
  // anything can edit.
}

void PresentationView::AttachFile(edit::Editor& ed) {
  if (fileAttached_) return;
  fileAttached_ = true;
  PresentationFile& f = File();
  ed.SetJournal(f.Journal());
  // After each edit, undo and redo: unsaved or not (the title's mark, the owner file, the autosave).
  ed.OnChange([this](const edit::Changes&) {
    edit::Editor* e = deck_ ? deck_->Editor() : nullptr;
    if (!e || !file_) return;
    const bool was = file_->Dirty();
    file_->Changed(*e);
    if (file_->Dirty() != was) host_.ViewStateChanged();
  });
}

bool PresentationView::ReadyToSave(std::wstring& error) {
  if (!Ready()) return false;
  edit::Editor* ed = deck_->Editor();
  if (!ed) {
    error = Title() + L" can't be saved.";
    return false;
  }
  // What is being typed is part of what's saved: notes still in the notes pane go to the slide (the
  // pane stays open), and typing on a slide ends its step.
  CommitNotes();  // the notes being typed (p2-slides), the box left open
  ed->EndTyping();
  return true;
}

void PresentationView::SavedTo() {
  path_ = file_->Path();
  host_.ViewStateChanged();
  host_.ViewInvalidate();
}

bool PresentationView::Save(const std::wstring& path, std::wstring& error) {
  if (!ReadyToSave(error)) return false;
  if (File().SaveRunning()) SaveDone();  // closing right after Ctrl+S: that save first
  if (!File().Save(*deck_->Editor(), path, error)) return false;
  SavedTo();
  return true;
}

TabContent::SaveStart PresentationView::StartSave(const std::wstring& path, std::wstring& error) {
  if (!ReadyToSave(error)) return SaveStart::Failed;
  HWND hwnd = host_.ViewHwnd();
  const LPARAM self = reinterpret_cast<LPARAM>(this);
  // The worker posts to the window when the file is written; SaveDone finishes on the UI thread.
  if (!File().StartSave(*deck_->Editor(), path, [hwnd, self] { PostMessageW(hwnd, kSaveDoneMessage, 0, self); }, error))
    return SaveStart::Failed;
  host_.ViewStateChanged();
  return SaveStart::Started;
}

void PresentationView::SaveDone() {
  if (!file_ || !file_->SaveRunning()) return;
  std::wstring error;
  const bool ok = file_->FinishSave(error);
  if (ok) SavedTo();
  else host_.ViewStateChanged();
  host_.ViewSaved(this, ok, error);
}

SaveFormat PresentationView::SaveAsFormat() const {
  const std::wstring named = path_.empty() ? SuggestedSavePath() : path_;
  for (const SaveFormat& f : kFormats)
    if (EndsWithNoCase(named, std::wstring(L".") + f.ext)) return f;
  return deck_ && deck_->HasMacros() ? kFormats[1] : kFormats[0];
}

std::vector<SaveFormat> PresentationView::OtherSaveFormats() const {
  // A deck with macros keeps them only in the macro-enabled formats (the writer refuses the others).
  const bool macros = deck_ && deck_->HasMacros();
  const std::wstring_view main = SaveAsFormat().ext;
  std::vector<SaveFormat> other;
  for (const SaveFormat& f : kFormats) {
    const bool macroFormat = std::wstring_view(f.ext).back() == L'm';
    if (f.ext != main && macroFormat == macros) other.push_back(f);
  }
  return other;
}

std::wstring PresentationView::SuggestedSavePath() const {
  const Presentation* deck = deck_ ? deck_->Model() : nullptr;
  return deck && file_ ? file_->SuggestedPath(*deck) : path_;
}

void PresentationView::Autosave() {
  edit::Editor* ed = Ready() ? deck_->Editor() : nullptr;
  if (ed && file_ && file_->Dirty()) file_->Autosave(*ed);
}

// A recovered journal: its part states made again through the editor, as one "Recover" step (the
// tab's own journal records them for this session's recovery file).
bool PresentationView::ReplayEdits(std::string_view records, std::wstring& error) {
  edit::Editor* ed = Ready() ? deck_->Editor() : nullptr;
  if (!ed) return false;
  if (records.empty()) return true;
  const edit::Result r = edit::Replay(*ed, records);
  if (r) return true;
  error = std::wstring(r.message.begin(), r.message.end());
  return false;
}

FileChange PresentationView::CheckFileOnDisk() {
  // Our own save changes the file: not another program's.
  if (!Ready() || !file_ || file_->SaveRunning()) return FileChange::None;
  return static_cast<FileChange>(file_->CheckDisk());
}

}  // namespace fw::slide::view
