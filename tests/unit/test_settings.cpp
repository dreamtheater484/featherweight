// Featherweight.ini: files written by earlier versions read the same, and save back unchanged.
// Each test uses its own file in the temporary folder, never the real settings.

#include <string>

#include "Settings.h"
#include "TestHarness.h"
#include "Util.h"

using fw::FileKind;

namespace {

// A temporary settings file holding `text`, deleted when the test is done.
class TempIni {
 public:
  explicit TempIni(const std::string& text) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    static int n = 0;
    path_ = std::wstring(dir) + L"fw-settings-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(++n) + L".ini";
    util::WriteFileAtomic(path_, text);
  }
  ~TempIni() { DeleteFileW(path_.c_str()); }
  const std::wstring& Path() const { return path_; }
  std::string Read() const {
    std::string s;
    util::ReadFileBytes(path_, s);
    return s;
  }

 private:
  std::wstring path_;
};

// What 0.9.4 writes.
const std::string kOldFile =
    "; Featherweight settings. Safe to delete.\r\n"
    "[General]\r\n"
    "Theme=dark\r\n"
    "InvertPages=1\r\n"
    "TwoPageCoverAlone=0\r\n"
    "HighlightFields=1\r\n"
    "UpdateAutomatically=0\r\n"
    "LastUpdateCheck=1761900000\r\n"
    "SkippedVersion=0.9.5\r\n"
    "LastRunVersion=0.9.4\r\n"
    "OfferedDefaultApp=1\r\n"
    "Window=-8,-8,1928,1048\r\n"
    "Maximized=1\r\n"
    "\r\n"
    "[Recent]\r\n"
    "1=C:\\Docs\\Lease.pdf|1761900000\r\n"
    "2=D:\\Scans\\Tax 2025.pdf|1761800000\r\n"
    "\r\n"
    "[State]\r\n"
    "0123456789abcdef=3,1,1.2500,0,0,1761900000\r\n";

}  // namespace

TEST(Settings_ReadsAndKeepsOldFiles) {
  TempIni file(kOldFile);
  Settings s;
  s.LoadFrom(file.Path());
  CHECK(s.theme == ThemeSetting::Dark);
  CHECK(s.invert);
  CHECK(!s.coverAlone);
  CHECK(s.highlightFields);
  CHECK(!s.autoUpdate);
  CHECK_EQ(s.lastUpdateCheck, 1761900000);
  CHECK(s.skippedVersion == L"0.9.5");
  CHECK(s.lastRunVersion == L"0.9.4");
  CHECK(s.offeredDefaultApp[static_cast<int>(FileKind::Pdf)]);
  CHECK(!s.offeredDefaultApp[static_cast<int>(FileKind::Document)]);
  CHECK(s.hasWindowRect && s.windowRect.left == -8 && s.windowRect.bottom == 1048);
  CHECK(s.maximized);
  CHECK_EQ(s.recent.size(), static_cast<size_t>(2));
  CHECK(s.recent[0].path == L"C:\\Docs\\Lease.pdf" && s.recent[0].lastOpened == 1761900000);
  CHECK(s.recent[1].Kind() == FileKind::Pdf);
  // Saved without changes, it is the same file byte for byte.
  CHECK(s.Save());
  CHECK(file.Read() == kOldFile);
}

TEST(Settings_RecentFilesOfEveryKind) {
  TempIni file(
      "[Recent]\r\n"
      "1=C:\\Work\\Report Q3.docx|1761900500\r\n"
      "2=C:\\Work\\Budget 2026.xlsx|1761900400\r\n"
      "3=C:\\Work\\Kickoff.pptx|1761900300\r\n"
      "4=C:\\Docs\\Lease.pdf|1761900200\r\n"
      "5=C:\\Docs\\notes.txt\r\n"
      "6=C:\\Docs\\photo.png|1761900100\r\n"
      "7=|1761900000\r\n");
  Settings s;
  s.LoadFrom(file.Path());
  const FileKind kinds[] = {FileKind::Document, FileKind::Workbook, FileKind::Presentation,
                            FileKind::Pdf,      FileKind::Document, FileKind::Unknown};
  CHECK_EQ(s.recent.size(), static_cast<size_t>(6));  // the entry without a path is skipped
  for (size_t i = 0; i < 6 && i < s.recent.size(); ++i) CHECK(s.recent[i].Kind() == kinds[i]);
  CHECK_EQ(s.recent[4].lastOpened, 0);  // an entry without a time
  // Opening one again moves it to the top, whatever its kind.
  s.AddRecent(L"C:\\Work\\BUDGET 2026.xlsx");
  CHECK_EQ(s.recent.size(), static_cast<size_t>(6));
  CHECK(s.recent[0].path == L"C:\\Work\\BUDGET 2026.xlsx" && s.recent[1].path == L"C:\\Work\\Report Q3.docx");
  CHECK(s.Save());
  Settings again;
  again.LoadFrom(file.Path());
  CHECK_EQ(again.recent.size(), static_cast<size_t>(6));
  for (size_t i = 0; i < 6 && i < again.recent.size(); ++i) CHECK(again.recent[i].path == s.recent[i].path);
}

TEST(Settings_KeepsTenRecentFiles) {
  std::string text = "[Recent]\r\n";
  for (int i = 1; i <= 14; ++i)
    text += std::to_string(i) + "=C:\\f" + std::to_string(i) + ".pdf|" + std::to_string(100 - i) + "\r\n";
  TempIni file(text);
  Settings s;
  s.LoadFrom(file.Path());
  CHECK_EQ(s.recent.size(), static_cast<size_t>(Settings::kMaxRecent));
  CHECK(s.recent.back().path == L"C:\\f10.pdf");
}

TEST(Settings_DefaultAppOfferPerKind) {
  TempIni file(kOldFile);
  Settings s;
  s.LoadFrom(file.Path());
  s.offeredDefaultApp[static_cast<int>(FileKind::Workbook)] = true;
  CHECK(s.Save());
  const std::string saved = file.Read();
  CHECK(saved.find("OfferedDefaultApp=1\r\n") != std::string::npos);  // PDF's 0.9 key
  CHECK(saved.find("OfferedDefaultAppWorkbook=1\r\n") != std::string::npos);
  CHECK(saved.find("OfferedDefaultAppDocument") == std::string::npos);  // only kinds that were offered
  Settings again;
  again.LoadFrom(file.Path());
  CHECK(again.offeredDefaultApp[static_cast<int>(FileKind::Pdf)]);
  CHECK(again.offeredDefaultApp[static_cast<int>(FileKind::Workbook)]);
  CHECK(!again.offeredDefaultApp[static_cast<int>(FileKind::Presentation)]);
}
