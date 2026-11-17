#include "sheet/view/WorkbookFile.h"

#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <new>
#include <thread>

#include "core/Strings.h"
#include "pkg/Zip.h"
#include "sheet/edit/Edit.h"
#include "sheet/xlsx/Xlsx.h"

namespace pkg = fw::pkg;
using fw::sheet::FileFormat;

struct WorkbookFile::Running {
  std::thread worker;
  SaveJob job;
  fw::sheet::xlsx::SavePlan plan;
  fw::sheet::Workbook* book = nullptr;
  fw::sheet::edit::Editor* editor = nullptr;
  pkg::Status written;
  pkg::SaveResult result;
};

WorkbookFile::WorkbookFile() : FileSession(L"Book", pkg::OwnerStyle::Excel) {}

WorkbookFile::~WorkbookFile() {
  if (run_ && run_->worker.joinable()) run_->worker.join();
}

bool WorkbookFile::IsTemplate(const std::wstring& path) {
  return fw::EndsWithNoCase(path, L".xltx") || fw::EndsWithNoCase(path, L".xltm");
}

void WorkbookFile::OpenedTemplate(const std::wstring& path, int number) {
  const size_t dot = path.rfind(L'.');
  const std::wstring stem = path.substr(0, dot);
  const std::wstring title = fw::FileNameOf(stem) + std::to_wstring(number);
  const wchar_t* ext = fw::EndsWithNoCase(path, L".xltm") ? L".xlsm" : L".xlsx";
  const size_t slash = stem.find_last_of(L"\\/");
  Copy(title, (slash == std::wstring::npos ? std::wstring() : stem.substr(0, slash + 1)) + title + ext, false);
}

FileFormat WorkbookFile::FormatOf(const std::wstring& path, FileFormat current) {
  if (fw::EndsWithNoCase(path, L".xlsx")) return FileFormat::Xlsx;
  if (fw::EndsWithNoCase(path, L".xlsm")) return FileFormat::Xlsm;
  if (fw::EndsWithNoCase(path, L".xltx")) return FileFormat::Xltx;
  if (fw::EndsWithNoCase(path, L".xltm")) return FileFormat::Xltm;
  return current;
}

std::wstring WorkbookFile::SuggestedPath(const fw::sheet::Workbook& book) const {
  if (GetOrigin() == Origin::File) return Path();
  if (GetOrigin() == Origin::Copy) return SuggestedBase();
  std::wstring dir;
  PWSTR docs = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
    dir = std::wstring(docs) + L"\\";
    CoTaskMemFree(docs);
  }
  const wchar_t* ext = book.Format() == FileFormat::Xlsm   ? L".xlsm"
                       : book.Format() == FileFormat::Xltx ? L".xltx"
                       : book.Format() == FileFormat::Xltm ? L".xltm"
                                                          : L".xlsx";
  return dir + Title() + ext;
}

bool WorkbookFile::StartSave(fw::sheet::Workbook& book, fw::sheet::edit::Editor& editor, const std::wstring& path,
                             std::function<void()> done, std::wstring& error) {
  if (run_) {
    error = Title() + L" is being saved already.";
    return false;
  }
  auto r = std::make_unique<Running>();
  if (!BeginSave(path, r->job, error)) return false;
  editor.BeginSave();
  pkg::Status st = fw::sheet::xlsx::PrepareSave(book, editor, r->plan, FormatOf(r->job.target, book.Format()));
  if (!st.Ok()) {
    editor.EndSave();
    error = r->job.name + L" couldn't be saved. " + fw::Utf8ToWide(st.message);
    return false;
  }
  r->book = &book;
  r->editor = &editor;
  Running* p = r.get();
  r->worker = std::thread([p, done = std::move(done)] {
    try {
      pkg::MemorySink sink;
      p->written = fw::sheet::xlsx::WriteSave(*p->book, p->plan, sink);
      if (p->written.Ok())
        p->result = FileSession::WriteSave(p->job, std::move(sink.bytes), &p->book->GetPackage()->GetSnapshot());
    } catch (const std::bad_alloc&) {
      p->written = pkg::Status::Fail(pkg::Status::Code::TooLarge, "There isn't enough memory to write it.");
    }
    if (done) done();
  });
  run_ = std::move(r);
  return true;
}

bool WorkbookFile::FinishSave(std::wstring& error) {
  if (!run_) {
    error = L"No save is running.";
    return false;
  }
  if (run_->worker.joinable()) run_->worker.join();
  std::unique_ptr<Running> r = std::move(run_);
  if (!r->written.Ok()) {
    r->editor->EndSave();
    error = r->job.name + L" couldn't be saved. " + fw::Utf8ToWide(r->written.message);
    return false;
  }
  if (!r->result.Ok()) {
    r->editor->EndSave();
    error = fw::Utf8ToWide(r->result.message);
    return false;
  }
  fw::sheet::xlsx::CommitSave(*r->book, *r->editor, r->plan);
  SaveDone(r->job, r->result);
  return true;
}

bool WorkbookFile::Save(fw::sheet::Workbook& book, fw::sheet::edit::Editor& editor, const std::wstring& path,
                        std::wstring& error) {
  return StartSave(book, editor, path, nullptr, error) && FinishSave(error);
}
