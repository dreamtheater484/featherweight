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
#include "pkg/Recovery.h"
#include "sheet/csv/Csv.h"
#include "sheet/edit/Edit.h"
#include "sheet/numfmt/NumLocale.h"
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
        WorkbookFile::FormatOf(L"x.txt", FileFormat::Xlsm) == FileFormat::Xlsm &&
        WorkbookFile::FormatOf(L"x.CSV", FileFormat::Xlsx) == FileFormat::Csv);
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

namespace {

std::vector<uint8_t> Bytes(std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::string ReadAll(const std::wstring& path) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open(path, {}, st);
  if (!snap) return {};
  const auto b = snap->Bytes();
  return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

std::unique_ptr<Workbook> OpenCsv(const std::wstring& path, const csv::Options& o, fw::pkg::Snapshot::Stamp& stamp) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open(path, {}, st);
  if (!snap) return nullptr;
  stamp = snap->GetStamp();
  const auto b = snap->Bytes();
  auto book = std::make_unique<Workbook>();
  csv::Source source;
  csv::Read(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), "Prices", o, *book, source);
  return book;
}

}  // namespace

// A CSV file saved keeps its own way of writing (no byte-order mark, ';', LF here); only the row
// edited is written anew; Save As .tsv writes a new tab-separated file; an Excel workbook saves
// CSV only as a copy.
TEST(WorkbookFile_CsvKeepsItsWayAndAWorkbookCopiesToCsv) {
  Folder f;
  const std::wstring path = f.path + L"\\Prices.csv";
  const std::string original = "Item;Price\n\"Tea\";2,50\nCoffee;3\n";
  CHECK(test::WriteFileBytes(path, Bytes(original)));
  csv::Options o;
  o.locale = &EnNlLocale();  // decimal comma, ";" between fields
  fw::pkg::Snapshot::Stamp stamp;
  auto book = OpenCsv(path, o, stamp);
  if (!book) return;
  CHECK(book->Format() == FileFormat::Csv);
  Editor ed(*book);
  WorkbookFile file;
  file.Opened(path, stamp);
  file.SetCsvOptions(o);
  ed.OnChange([&](const std::vector<edit::Change>&) { file.Changed(ed.Modified(), {}); });
  std::wstring error;
  // Untouched: the same bytes.
  CHECK(file.Save(*book, ed, path, error));
  CHECK(ReadAll(path) == original);
  // One row edited: the others as they were.
  CHECK(ed.SetCell(0, 2, 0, CellInput::Text("Cocoa")));
  CHECK(file.Dirty() && ed.Modified());
  CHECK(file.Save(*book, ed, path, error));
  CHECK(ReadAll(path) == "Item;Price\n\"Tea\";2,50\nCocoa;3\n");
  CHECK(!file.Dirty() && !ed.Modified() && book->Format() == FileFormat::Csv);
  CHECK_EQ(f.Count(L"Backups"), 1u);
  // Save As .tsv: a new file in Excel's way for it, and the tab is that file.
  const std::wstring tsv = f.path + L"\\Prices.tsv";
  CHECK(file.Save(*book, ed, tsv, error));
  CHECK(ReadAll(tsv) == "\xEF\xBB\xBFItem\tPrice\r\nTea\t2,50\r\nCocoa\t3\r\n");  // 2,50 read with 0.00
  CHECK(file.Path() == tsv);
  // Save As .xlsx (the banner's choice): an Excel workbook with the edits, sorted, and the tab is it.
  CHECK(ed.SortRows(0, Range{1, 0, 2, 1}, {1, 0}, {{0, false}}));  // Cocoa, Tea
  const std::wstring xlsx = f.path + L"\\Prices.xlsx";
  CHECK(file.Save(*book, ed, xlsx, error));
  CHECK(file.Path() == xlsx && !file.Dirty() && book->Format() == FileFormat::Xlsx);
  auto reopened = OpenFile(xlsx);
  CHECK(reopened && reopened->GetSheet(0).Name() == "Prices");
  if (reopened) {
    CHECK(reopened->GetSheet(0).GetValue(1, 0).text == "Cocoa" && reopened->GetSheet(0).GetValue(2, 1).number == 2.5);
    CHECK(reopened->GetSheet(0).Text().afterData.find("<sortState ref=\"A2:B3\">") != std::string::npos);
  }

  // An Excel workbook: Save as .csv is refused, a copy is written.
  const std::wstring xlsxPath = f.path + L"\\Budget.xlsx";
  CHECK(test::WriteFileBytes(xlsxPath, test::MakeZip(sheet_test::WorkbookParts({"<sheetData/>"}))));
  fw::pkg::Snapshot::Stamp xStamp;
  auto workbook = OpenFile(xlsxPath, &xStamp);
  if (!workbook) return;
  Editor xed(*workbook);
  WorkbookFile xfile;
  xfile.Opened(xlsxPath, xStamp);
  xfile.SetCsvOptions(o);
  CHECK(xed.SetCell(0, 0, 0, CellInput::Text("a,b")));
  CHECK(xed.SetCell(0, 0, 1, CellInput::Number(1.5)));
  CHECK(!xfile.StartSave(*workbook, xed, f.path + L"\\Budget.csv", nullptr, error));
  CHECK(error.find(L"Save a copy as CSV") != std::wstring::npos && !xed.Saving());
  CHECK(WorkbookFile::SaveCsvCopy(*workbook, f.path + L"\\Budget.csv", o, error));
  CHECK(ReadAll(f.path + L"\\Budget.csv") == "\xEF\xBB\xBF" "a,b;1,5\r\n");
  CHECK(xfile.Path() == xlsxPath && xed.Modified());
}

// The autosave timer (s2-grid's tab): unsaved edits go to the recovery folder as a save would
// write them, the workbook stays unsaved and editable, and the autosave reads back as the workbook
// with the edits, titled as the tab ("Book2").
TEST(WorkbookFile_AutosaveHoldsTheEditsAndLeavesTheWorkbookUnsaved) {
  Folder f;
  fw::pkg::Status st;
  auto book = xlsx::NewWorkbook(st);
  CHECK(book != nullptr);
  if (!book) return;
  WorkbookFile file;
  file.New(2);
  Editor ed(*book);
  const std::wstring pattern = WorkbookFile::Config().recovery + L"\\*.fwr";
  auto first = [&](std::wstring& name) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    name = WorkbookFile::Config().recovery + L"\\" + fd.cFileName;
    return true;
  };
  std::wstring name;
  file.Autosave(*book, ed);
  CHECK(!first(name));  // nothing unsaved yet
  CHECK(ed.SetCell(0, 0, 0, CellInput::Number(42)));
  file.Changed(ed.Modified(), {});
  CHECK(file.Dirty() && file.AutosaveDue());
  file.Autosave(*book, ed);
  CHECK(!file.AutosaveDue() && file.Dirty() && ed.Modified() && !ed.Saving());
  CHECK(ed.SetCell(0, 0, 1, CellInput::Number(7)));  // still editable
  CHECK(first(name));
  fw::pkg::recovery::Entry entry;
  std::vector<uint8_t> bytes;
  CHECK(fw::pkg::recovery::Read(name, entry, bytes));
  CHECK(entry.title == L"Book2" && entry.originalPath.empty());
  auto snap = fw::pkg::Snapshot::FromBytes(std::move(bytes));
  auto pkg = fw::pkg::Package::Open(std::move(snap), fw::pkg::Limits::Default(), st);
  auto recovered = pkg ? xlsx::Read(std::move(pkg), st, {.loadAll = true}) : nullptr;
  CHECK(recovered != nullptr);
  if (recovered) CHECK_EQ(sheet_test::Text(recovered->GetSheet(0), 0, 0), std::string("42"));
  WorkbookFile::RecoverySession().End();
}
