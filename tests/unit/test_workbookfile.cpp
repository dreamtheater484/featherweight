// The file behind a workbook tab (S2 "Saving"): saves on a worker thread while the workbook is
// read-only, keeps the original once in the backups, writes Excel's owner file only while there
// are unsaved edits, refuses to replace a file another program changed, and a new workbook saves
// like any other.
#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "SheetTestUtil.h"
#include "TestHarness.h"
#include "pkg/Check.h"
#include "pkg/OwnerFile.h"
#include "sheet/edit/Edit.h"
#include "sheet/view/WorkbookFile.h"
#include "sheet/xlsx/Xlsx.h"

using namespace fw::sheet;
using edit::CellInput;
using edit::Editor;

namespace {

struct Folder {
  std::wstring path = test::TempPath(L"workbookfile");
  Folder() {
    CreateDirectoryW(path.c_str(), nullptr);
    WorkbookFile::Config() = {path + L"\\Backups", path + L"\\Recovery", L"Tester", L"en-US", false};
  }
  ~Folder() {
    WorkbookFile::Config() = {};
    Remove(path);
  }
  static void Remove(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        std::wstring name = fd.cFileName, p = dir + L"\\" + name;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          Remove(p);
        } else {
          SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
          DeleteFileW(p.c_str());
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
  }
  size_t Count(const std::wstring& sub) const {
    size_t n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\" + sub + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do n += !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
  }
};

std::unique_ptr<Workbook> OpenFile(const std::wstring& path, fw::pkg::Snapshot::Stamp* stamp = nullptr) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open(path, {}, st);
  if (snap && stamp) *stamp = snap->GetStamp();
  auto pkg = snap ? fw::pkg::Package::Open(std::move(snap), fw::pkg::Limits::Default(), st) : nullptr;
  return pkg ? xlsx::Read(std::move(pkg), st, {.loadAll = true}) : nullptr;
}

bool CheckClean(const std::wstring& path) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open(path, {}, st);
  auto pkg = snap ? fw::pkg::Package::Open(std::move(snap), fw::pkg::Limits::Default(), st) : nullptr;
  if (!pkg) return false;
  fw::pkg::CheckReport report = fw::pkg::CheckPackage(*pkg);
  for (const std::string& e : report.errors) std::printf("  check: %s\n", e.c_str());
  return report.Ok();
}

}  // namespace

TEST(WorkbookFile_SavesOnAWorkerKeepsTheOriginalOnceAndTellsExcel) {
  Folder f;
  const std::wstring path = f.path + L"\\Budget.xlsx";
  CHECK(test::WriteFileBytes(path, test::MakeZip(sheet_test::WorkbookParts({"<sheetData/>", "<sheetData/>"}))));
  fw::pkg::Snapshot::Stamp stamp;
  auto book = OpenFile(path, &stamp);
  if (!book) return;
  Editor ed(*book);
  WorkbookFile file;
  file.Opened(path, stamp);
  ed.OnChange([&](const std::vector<edit::Change>&) { file.Changed(ed.Modified(), {}); });
  CHECK(ed.SetCell(0, 0, 0, CellInput::Text("Rent")));
  CHECK(file.Dirty() && fw::pkg::ReadOwner(path, fw::pkg::OwnerStyle::Excel) == L"Tester");

  std::atomic<bool> done{false};
  std::wstring error;
  CHECK(file.StartSave(*book, ed, path, [&] { done = true; }, error));
  CHECK(file.SaveRunning() && ed.Saving() && !ed.SetCell(0, 0, 1, CellInput::Number(1)));
  CHECK(file.FinishSave(error));
  CHECK(done && !file.SaveRunning() && !ed.Saving() && !file.Dirty() && !ed.Modified());
  CHECK(fw::pkg::ReadOwner(path, fw::pkg::OwnerStyle::Excel).empty());
  CHECK_EQ(f.Count(L"Backups"), 1u);
  CHECK(CheckClean(path));
  auto saved = OpenFile(path);
  if (!saved) return;
  CHECK_EQ(sheet_test::Text(saved->GetSheet(0), 0, 0), std::string("Rent"));

  // Again: our own file is replaced, the original stays the only backup.
  CHECK(ed.SetCell(0, 1, 0, CellInput::Number(950)));
  CHECK(file.Save(*book, ed, path, error));
  CHECK_EQ(f.Count(L"Backups"), 1u);
  saved = OpenFile(path);
  if (!saved) return;
  CHECK_EQ(sheet_test::Text(saved->GetSheet(0), 0, 0), std::string("Rent"));
  CHECK_EQ(sheet_test::Text(saved->GetSheet(0), 1, 0), std::string("950"));
}

TEST(WorkbookFile_RefusesAFileChangedOnDisk) {
  Folder f;
  const std::wstring path = f.path + L"\\Shared.xlsx";
  const auto bytes = test::MakeZip(sheet_test::WorkbookParts({"<sheetData/>", "<sheetData/>"}));
  CHECK(test::WriteFileBytes(path, bytes));
  fw::pkg::Snapshot::Stamp stamp;
  auto book = OpenFile(path, &stamp);
  if (!book) return;
  Editor ed(*book);
  WorkbookFile file;
  file.Opened(path, stamp);
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(1)));
  std::vector<uint8_t> other = bytes;
  other.push_back(0);  // someone else saved it
  CHECK(test::WriteFileBytes(path, other));
  std::wstring error;
  CHECK(!file.StartSave(*book, ed, path, nullptr, error));
  CHECK(error.find(L"changed by another program") != std::wstring::npos);
  CHECK(!ed.Saving() && ed.Modified() && !file.SaveRunning());
  // Save As elsewhere works.
  CHECK(file.Save(*book, ed, f.path + L"\\Copy.xlsx", error));
  CHECK(file.Path() == f.path + L"\\Copy.xlsx" && !ed.Modified());
}

TEST(WorkbookFile_ANewWorkbookSavesAsAnyOther) {
  Folder f;
  fw::pkg::Status st;
  auto book = xlsx::NewWorkbook(st);
  CHECK(book != nullptr);
  if (!book) return;
  CHECK(book->SheetCount() == 1 && book->GetSheet(0).Name() == "Sheet1" && book->Format() == FileFormat::Xlsx);
  WorkbookFile file;
  file.New(1);
  CHECK(file.Title() == L"Book1" && file.SuggestedPath(*book).ends_with(L"\\Book1.xlsx"));
  Editor ed(*book);
  CHECK(ed.SetCell(0, 0, 0, CellInput::Text("Item")));
  CHECK(ed.SetCell(0, 0, 1, CellInput::Text("Amount")));
  CHECK(ed.SetCell(0, 1, 0, CellInput::Text("Rent")));
  CHECK(ed.SetCell(0, 1, 1, CellInput::Number(950)));
  CHECK(ed.SetCell(0, 2, 1, CellInput::Formula("SUM(B2:B2)")));
  CHECK(ed.AddSheet(-1, "Notes"));
  std::wstring error;
  const std::wstring path = f.path + L"\\Book1.xlsm";
  CHECK(file.Save(*book, ed, path, error));
  CHECK(CheckClean(path) && file.Title() == L"Book1.xlsm" && book->Format() == FileFormat::Xlsm);
  auto saved = OpenFile(path);
  if (!saved) return;
  CHECK(saved->Format() == FileFormat::Xlsm && saved->SheetCount() == 2);
  CHECK_EQ(sheet_test::Text(saved->GetSheet(0), 1, 1), std::string("950"));
  CHECK_EQ(sheet_test::FormulaText(saved->GetSheet(0), 2, 1), std::string("SUM(B2:B2)"));
  CHECK(WorkbookFile::FormatOf(L"x.XLTX", FileFormat::Xlsx) == FileFormat::Xltx &&
        WorkbookFile::FormatOf(L"x.csv", FileFormat::Xlsm) == FileFormat::Xlsm);
  // A template opens as a new workbook: Save goes to Save As, suggesting a workbook beside it.
  WorkbookFile fromTemplate;
  CHECK(WorkbookFile::IsTemplate(L"C:\\t\\Budget.XLTX") && !WorkbookFile::IsTemplate(L"C:\\t\\Budget.xlsx"));
  fromTemplate.OpenedTemplate(L"C:\\t\\Budget.xltm", 1);
  CHECK(fromTemplate.Title() == L"Budget1" && fromTemplate.GetOrigin() == FileSession::Origin::Copy);
  CHECK(fromTemplate.SuggestedPath(*book) == L"C:\\t\\Budget1.xlsm" && fromTemplate.Path().empty());
  // For the Excel open check: FW_SAVE_OUT keeps a copy.
  wchar_t out[MAX_PATH];
  if (DWORD n = GetEnvironmentVariableW(L"FW_SAVE_OUT", out, MAX_PATH); n > 0 && n < MAX_PATH)
    CopyFileW(path.c_str(), (std::wstring(out) + L"\\new-book.xlsm").c_str(), FALSE);
}
