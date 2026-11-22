#include "slide/view/PresentationFile.h"

#include <windows.h>
#include <shlobj.h>

#include <new>
#include <thread>

#include "core/Strings.h"
#include "pkg/Zip.h"
#include "slide/edit/Save.h"

namespace pkg = fw::pkg;
using fw::slide::FileFormat;

struct PresentationFile::Running {
  std::thread worker;
  SaveJob job;
  fw::slide::edit::SavePlan plan;
  fw::slide::edit::Editor* editor = nullptr;
  pkg::Status written;
  pkg::SaveResult result;
};

PresentationFile::PresentationFile() : FileSession(L"Presentation", fw::pkg::OwnerStyle::Word) {}

PresentationFile::~PresentationFile() {
  if (run_ && run_->worker.joinable()) run_->worker.join();
}

FileFormat PresentationFile::FormatOf(const std::wstring& path, FileFormat current) {
  if (fw::EndsWithNoCase(path, L".pptx")) return FileFormat::Pptx;
  if (fw::EndsWithNoCase(path, L".pptm")) return FileFormat::Pptm;
  if (fw::EndsWithNoCase(path, L".ppsx")) return FileFormat::Ppsx;
  if (fw::EndsWithNoCase(path, L".ppsm")) return FileFormat::Ppsm;
  if (fw::EndsWithNoCase(path, L".potx")) return FileFormat::Potx;
  if (fw::EndsWithNoCase(path, L".potm")) return FileFormat::Potm;
  return current;
}

std::wstring PresentationFile::SuggestedPath(const fw::slide::Presentation& deck) const {
  if (GetOrigin() == Origin::File) return Path();
  if (GetOrigin() == Origin::Copy) return SuggestedBase();
  std::wstring dir;
  PWSTR docs = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
    dir = std::wstring(docs) + L"\\";
    CoTaskMemFree(docs);
  }
  // A template or a show opens as a presentation: Save As suggests one (with macros: .pptm).
  return dir + Title() + (deck.HasMacros() ? L".pptm" : L".pptx");
}

bool PresentationFile::StartSave(fw::slide::edit::Editor& editor, const std::wstring& path, std::function<void()> done,
                                 std::wstring& error) {
  if (run_) {
    error = Title() + L" is being saved already.";
    return false;
  }
  auto r = std::make_unique<Running>();
  if (!BeginSave(path, r->job, error)) return false;
  const fw::slide::Presentation& deck = editor.Deck();
  pkg::Status st = fw::slide::edit::PrepareSave(editor, r->plan, FormatOf(r->job.target, deck.Format()));
  if (!st.Ok()) {
    error = r->job.name + L" couldn't be saved. " + fw::Utf8ToWide(st.message);
    return false;
  }
  r->editor = &editor;
  Running* p = r.get();
  const pkg::Snapshot* opened = &deck.GetPackage().GetSnapshot();
  r->worker = std::thread([p, opened, done = std::move(done)] {
    try {
      pkg::MemorySink sink;
      p->written = fw::slide::edit::WriteSave(p->plan, sink);
      if (p->written.Ok()) p->result = FileSession::WriteSave(p->job, std::move(sink.bytes), opened);
    } catch (const std::bad_alloc&) {
      p->written = pkg::Status::Fail(pkg::Status::Code::TooLarge, "There isn't enough memory to write it.");
    }
    if (done) done();
  });
  run_ = std::move(r);
  return true;
}

bool PresentationFile::FinishSave(std::wstring& error) {
  if (!run_) {
    error = L"No save is running.";
    return false;
  }
  if (run_->worker.joinable()) run_->worker.join();
  std::unique_ptr<Running> r = std::move(run_);
  if (!r->written.Ok()) {
    error = r->job.name + L" couldn't be saved. " + fw::Utf8ToWide(r->written.message);
    return false;
  }
  if (!r->result.Ok()) {
    error = fw::Utf8ToWide(r->result.message);
    return false;
  }
  fw::slide::edit::CommitSave(*r->editor, r->plan);
  SaveDone(r->job, r->result);
  Changed(*r->editor);  // edited while it saved: still unsaved
  return true;
}

bool PresentationFile::Save(fw::slide::edit::Editor& editor, const std::wstring& path, std::wstring& error) {
  if (!StartSave(editor, path, nullptr, error)) return false;
  return FinishSave(error);
}

void PresentationFile::Changed(const fw::slide::edit::Editor& editor) {
  FileSession::Changed(editor.Modified(), {});
  if (editor.Modified()) Edited(0);
}

void PresentationFile::Autosave(fw::slide::edit::Editor& editor) {
  if (run_ || !editor.Modified() || !AutosaveDue()) return;
  const auto bytes = editor.Deck().GetPackage().GetSnapshot().Bytes();
  if (WriteJournal(std::span<const uint8_t>(bytes.data(), bytes.size()), "pptx", journal_.All(editor),
                   journal_.Unwritten(editor)))
    journal_.Written();
}
