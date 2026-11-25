// F1, f1-trust: local crash reports (src/CrashReport). A dump of this process for an exception the
// test catches, the note the next start reads, pruning to the newest dumps, and the prefilled issue.
// Everything happens in a folder of its own in the temporary folder.

#include <string>
#include <vector>

#include "CrashReport.h"
#include "TestHarness.h"

namespace {

class TempFolder {
 public:
  TempFolder() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    static int n = 0;
    path_ = std::wstring(dir) + L"fw-crash-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n);
    // Not created: WriteDump makes it, as it does the first time on a user's PC.
  }
  ~TempFolder() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path_ + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((path_ + L"\\" + fd.cFileName).c_str());
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    RemoveDirectoryW(path_.c_str());
  }
  const std::wstring& Path() const { return path_; }

 private:
  std::wstring path_;
};

std::vector<std::wstring> Files(const std::wstring& folder, const wchar_t* pattern) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((folder + L"\\" + pattern).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do out.push_back(fd.cFileName);
  while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

// An exception raised and caught here; the filter writes the dump, as the crash handler does.
bool DumpCaught(const std::wstring& folder, const std::wstring& version) {
  bool ok = false;
  __try {
    RaiseException(0xE0001234, 0, 0, nullptr);
  } __except (ok = crash::WriteDump(folder, GetExceptionInformation(), GetCurrentThreadId(), version),
              EXCEPTION_EXECUTE_HANDLER) {
  }
  return ok;
}

void WriteFileWithTime(const std::wstring& path, int minutesAgo) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER t{{now.dwLowDateTime, now.dwHighDateTime}};
  t.QuadPart -= static_cast<ULONGLONG>(minutesAgo) * 60 * 10000000ull;
  FILETIME ft{t.LowPart, t.HighPart};
  SetFileTime(f, nullptr, nullptr, &ft);
  CloseHandle(f);
}

}  // namespace

TEST(f1_trust_crash_dump_and_note) {
  TempFolder dir;
  const std::wstring version = L"9.9.9";
  CHECK(DumpCaught(dir.Path(), version));
  const auto dumps = Files(dir.Path(), L"*.dmp");
  CHECK_EQ(dumps.size(), size_t(1));
  CHECK(dumps.size() == 1 && dumps[0].rfind(L"Featherweight-9.9.9-", 0) == 0);

  const auto pending = crash::TakePending(dir.Path());
  CHECK(pending.has_value());
  if (pending) {
    CHECK(pending->version == L"9.9.9");
    CHECK(pending->code == L"0xE0001234");
    CHECK(!pending->module.empty() && pending->module.find(L'\\') == std::wstring::npos);
    CHECK(pending->offset.rfind(L"0x", 0) == 0);
    CHECK(pending->windows.rfind(L"10.0.", 0) == 0);
    CHECK(dumps.size() == 1 && pending->dump == dumps[0]);
  }
  // The note is read once: the next start finds nothing; the dump stays for the user.
  CHECK(!crash::TakePending(dir.Path()).has_value());
  CHECK_EQ(Files(dir.Path(), L"*.dmp").size(), size_t(1));
}

TEST(f1_trust_crash_prune_keeps_newest) {
  TempFolder dir;
  CreateDirectoryW(dir.Path().c_str(), nullptr);
  for (int i = 0; i < 7; ++i) WriteFileWithTime(dir.Path() + L"\\old-" + std::to_wstring(i) + L".dmp", 10 * (i + 1));
  WriteFileWithTime(dir.Path() + L"\\note.txt", 100);  // not a dump: kept
  crash::Prune(dir.Path(), 5);
  const auto left = Files(dir.Path(), L"*.dmp");
  CHECK_EQ(left.size(), size_t(5));
  for (const auto& name : left) CHECK(name != L"old-5.dmp" && name != L"old-6.dmp");  // the two oldest
  CHECK_EQ(Files(dir.Path(), L"note.txt").size(), size_t(1));
}

TEST(f1_trust_crash_note_rejects_paths) {
  TempFolder dir;
  CreateDirectoryW(dir.Path().c_str(), nullptr);
  const std::wstring note = L"\xFEFFversion=1.0.0\r\ncode=0xC0000005\r\nmodule=x.dll\r\noffset=0x10\r\ndump=..\\..\\evil.dmp\r\n";
  HANDLE f = CreateFileW((dir.Path() + L"\\pending.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  DWORD written = 0;
  WriteFile(f, note.data(), static_cast<DWORD>(note.size() * sizeof(wchar_t)), &written, nullptr);
  CloseHandle(f);
  const auto pending = crash::TakePending(dir.Path());
  CHECK(pending.has_value() && pending->dump.empty() && pending->module == L"x.dll");
  CHECK(!crash::TakePending(L"").has_value());
}

TEST(f1_trust_issue_url) {
  crash::Pending p;
  p.version = L"4.1.0";
  p.code = L"0xC0000005";
  p.module = L"pdfium.dll";
  p.offset = L"0x1A2B";
  p.dump = L"Featherweight-4.1.0-20261125-101500.dmp";
  const std::wstring url = crash::IssueUrl(&p, L"4.1.0");
  CHECK(url.rfind(L"https://github.com/dreamtheater484/featherweight/issues/new?title=Crash%3A%200xC0000005%20in%20pdfium.dll&body=", 0) == 0);
  CHECK(url.find(L"pdfium.dll%2B0x1A2B") != std::wstring::npos);
  CHECK(url.find(L"Featherweight-4.1.0-20261125-101500.dmp") != std::wstring::npos);
  // Only unreserved characters and escapes: nothing a browser or GitHub would read differently.
  for (wchar_t c : url.substr(url.find(L'?') + 1))
    CHECK((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || wcschr(L"-_.~%=&", c));

  const std::wstring plain = crash::IssueUrl(nullptr, L"4.1.0");
  CHECK(plain.rfind(L"https://github.com/dreamtheater484/featherweight/issues/new?body=", 0) == 0);
  CHECK(plain.find(L"4.1.0") != std::wstring::npos);
  CHECK(crash::HelpUrl() == L"https://dreamtheater484.github.io/featherweight/help/");
}
