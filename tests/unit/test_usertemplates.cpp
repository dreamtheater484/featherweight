// The user's own templates (S4): what File → New lists from a personal templates folder. A
// temporary folder only; the user's real folders aren't touched.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "TestHarness.h"
#include "UserTemplates.h"
#include "core/FileKinds.h"
#include "core/Strings.h"

namespace {

std::vector<std::wstring> Names(const std::vector<std::wstring>& paths) {
  std::vector<std::wstring> out;
  for (const std::wstring& p : paths) out.push_back(fw::FileNameOf(p));
  return out;
}

}  // namespace

TEST(UserTemplates_ListsTheTemplatesShippedEditorsOpen) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) / (L"fw-usertemplates-" + std::to_wstring(GetCurrentProcessId()));
  fs::create_directories(dir / L"sub.xltx", ec);  // a folder named like one: not listed
  for (const wchar_t* n : {L"budget.xltx", L"Letter.dotx", L"notes.txt", L"~$draft.dotx", L"Invoice.OTS", L"cv.ott"})
    std::ofstream(dir / n) << "x";
  const bool docs = fw::EditorShipped(fw::FileKind::Document), books = fw::EditorShipped(fw::FileKind::Workbook);
  fw::SetEditorShippedForTesting(fw::FileKind::Document, true);
  fw::SetEditorShippedForTesting(fw::FileKind::Workbook, true);
  const std::vector<std::wstring> all = usertemplates::List(16, {dir.wstring()});
  CHECK((Names(all) == std::vector<std::wstring>{L"budget.xltx", L"cv.ott", L"Invoice.OTS", L"Letter.dotx"}));
  CHECK(all.size() == 4 && all[0] == (dir / L"budget.xltx").wstring());
  CHECK(usertemplates::List(2, {dir.wstring()}).size() == 2);
  // Without the workbook editor, only the documents' templates.
  fw::SetEditorShippedForTesting(fw::FileKind::Workbook, false);
  if (!books) CHECK((Names(usertemplates::List(16, {dir.wstring()})) == std::vector<std::wstring>{L"cv.ott", L"Letter.dotx"}));
  // A folder that isn't there lists nothing.
  CHECK(usertemplates::List(16, {(dir / L"missing").wstring()}).empty());
  fw::SetEditorShippedForTesting(fw::FileKind::Document, docs);
  fw::SetEditorShippedForTesting(fw::FileKind::Workbook, books);
  fs::remove_all(dir, ec);
}

TEST(UserTemplates_FoldersEndWithOfficesDefault) {
  const std::vector<std::wstring> f = usertemplates::Folders();
  CHECK(!f.empty() && fw::EndsWithNoCase(f.back(), L"\\Custom Office Templates"));
}
