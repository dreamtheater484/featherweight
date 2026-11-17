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
  bool csv = false;                   // a CSV or TSV file: csv::Write rather than the plan
  fw::sheet::csv::Options csvOptions;
  std::wstring follow;                // the CSV file the workbook stands for, whose way of writing is kept
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
  if (fw::EndsWithNoCase(path, L".csv") || fw::EndsWithNoCase(path, L".tsv")) return FileFormat::Csv;
  return current;
}

bool WorkbookFile::SaveCsvCopy(const fw::sheet::Workbook& book, const std::wstring& path,
                               const fw::sheet::csv::Options& options, std::wstring& error) {
  fw::sheet::csv::Options o = options;
  o.tsv = fw::EndsWithNoCase(path, L".tsv");
  o.delimiter = 0;
  const std::string bytes = fw::sheet::csv::WriteNew(book, o);
  const pkg::SaveResult r =
      pkg::SafeWrite(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  if (!r.Ok()) {
    error = fw::Utf8ToWide(r.message);
    return false;
  }
  return true;
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
  if (FormatOf(r->job.target, book.Format()) == FileFormat::Csv) return StartCsv(std::move(r), book, editor, done, error);
  // A workbook read from CSV has no package yet: the smallest one, which the save fills from the model.
  if (!book.GetPackage()) {
    if (const pkg::Status st = fw::sheet::xlsx::GiveBasePackage(book); !st.Ok()) {
      error = r->job.name + L" couldn't be saved. " + fw::Utf8ToWide(st.message);
      return false;
    }
  }
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

bool WorkbookFile::StartCsv(std::unique_ptr<Running> r, fw::sheet::Workbook& book, fw::sheet::edit::Editor& editor,
                            std::function<void()> done, std::wstring& error) {
  if (book.Format() != FileFormat::Csv) {
    // An Excel workbook keeps its own file; CSV is a copy of its first sheet's values.
    error = r->job.name + L" would keep only the values of the first sheet. Save a copy as CSV instead.";
    return false;
  }
  editor.BeginSave();
  r->csv = true;
  r->csvOptions = csv_;
  // The file the workbook stands for is followed when the new one is the same kind (.csv or .tsv).
  if (!Path().empty() && fw::EndsWithNoCase(Path(), L".tsv") == fw::EndsWithNoCase(r->job.target, L".tsv"))
    r->follow = Path();
  r->book = &book;
  r->editor = &editor;
  Running* p = r.get();
  r->worker = std::thread([p, done = std::move(done)] {
    try {
      std::unique_ptr<pkg::Snapshot> original;
      pkg::Status st;
      if (!p->follow.empty()) original = pkg::Snapshot::Open(p->follow, {}, st);
      std::string bytes;
      if (original) {
        const auto b = original->Bytes();
        bytes = fw::sheet::csv::Write(*p->book, std::string_view(reinterpret_cast<const char*>(b.data()), b.size()),
                                      p->csvOptions);
      } else {
        fw::sheet::csv::Options o = p->csvOptions;
        o.tsv = fw::EndsWithNoCase(p->job.target, L".tsv");
        o.delimiter = 0;
        bytes = fw::sheet::csv::WriteNew(*p->book, o);
      }
      p->result = FileSession::WriteSave(p->job, std::vector<uint8_t>(bytes.begin(), bytes.end()), original.get());
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
  if (r->csv && r->written.Ok() && r->result.Ok()) {
    // The file has the workbook's values. Saved, but every edit still counts for a later .xlsx save
    // (this ends the save, too).
    r->editor->SavedOutside(FileFormat::Csv);
    csv_.tsv = fw::EndsWithNoCase(r->job.target, L".tsv");
    SaveDone(r->job, r->result);
    return true;
  }
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

void WorkbookFile::Autosave(fw::sheet::Workbook& book, fw::sheet::edit::Editor& editor) {
  if (run_ || !AutosaveDue()) return;
  try {
    if (book.Format() == FileFormat::Csv) {
      fw::sheet::csv::Options o = csv_;
      o.delimiter = 0;
      const std::string bytes = fw::sheet::csv::WriteNew(book, o);
      WriteAutosave(std::vector<uint8_t>(bytes.begin(), bytes.end()));
      return;
    }
  } catch (const std::bad_alloc&) {
    return;
  }
  fw::sheet::xlsx::SavePlan plan;
  pkg::MemorySink sink;
  bool ok = false;
  editor.BeginSave();
  try {
    ok = fw::sheet::xlsx::PrepareSave(book, editor, plan).Ok() && fw::sheet::xlsx::WriteSave(book, plan, sink).Ok();
  } catch (const std::bad_alloc&) {
  }
  editor.EndSave();  // not committed: the workbook still stands for its file
  if (ok) WriteAutosave(std::move(sink.bytes));
}
