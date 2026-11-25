// Windows' share sheet (F1, f1-pdf): what a tab shares. The sheet itself is ShareSheet.cpp.
#include "Share.h"

#include "TabContent.h"
#include "core/Strings.h"
#include "pdf/PdfText.h"
#include "pdf/PdfView.h"

namespace share {

bool ShareTab(HWND hwnd, TabContent& tab, bool asPdf, std::wstring& message) {
  if (!tab.Ready()) return false;
  std::wstring base = fw::FileNameOf(tab.Path().empty() ? tab.Title() : tab.Path());
  if (const size_t dot = base.rfind(L'.'); dot != std::wstring::npos && dot > 0) base.resize(dot);
  if (base.empty()) base = T(L"Document");
  std::wstring path;
  if (asPdf && tab.Kind() != TabKind::Pdf) {
    if (!tab.CanExportPdf()) {
      message = T(L"This can't be made into a PDF.");
      return false;
    }
    path = TempFolder() + L"\\" + base + L".pdf";
    if (!tab.ExportPdf(path, message)) return false;
  } else if (PdfView* pdf = tab.AsPdf(); pdf && (tab.Modified() || tab.Path().empty())) {
    path = TempFolder() + L"\\" + base + L".pdf";
    if (!pdf->WriteCopy(path, message)) return false;
  } else if (tab.Path().empty() || tab.Modified()) {
    message = T(L"Save the file first: Share sends it as it's saved.");
    return false;
  } else {
    path = tab.Path();
  }
  if (!ShareFile(hwnd, path, fw::FileNameOf(path))) {
    message = T(L"Windows couldn't open its share sheet here.");
    return false;
  }
  message.clear();
  return true;
}

std::wstring TempFolder() {
  wchar_t temp[MAX_PATH + 1] = {};
  std::wstring dir(temp, GetTempPathW(MAX_PATH + 1, temp));
  if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
  dir += L"Featherweight share";  // lang: no
  CreateDirectoryW(dir.c_str(), nullptr);
  // Copies from earlier shares: a day is plenty for any app to have read them.
  FILETIME nowTime;
  GetSystemTimeAsFileTime(&nowTime);
  const ULONGLONG now = (static_cast<ULONGLONG>(nowTime.dwHighDateTime) << 32) | nowTime.dwLowDateTime;
  constexpr ULONGLONG kDay = 24ull * 3600 * 10000000;  // in 100 ns
  WIN32_FIND_DATAW f;
  const HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      const ULONGLONG written =
          (static_cast<ULONGLONG>(f.ftLastWriteTime.dwHighDateTime) << 32) | f.ftLastWriteTime.dwLowDateTime;
      if (now > written && now - written > kDay) DeleteFileW((dir + L"\\" + f.cFileName).c_str());
    } while (FindNextFileW(find, &f));
    FindClose(find);
  }
  return dir;
}

}  // namespace share
