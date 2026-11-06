// Safe saving (plan §3 "Never damage a file", §10): every way a save can fail leaves the original
// exactly as it was, with nothing left behind, and a good save keeps the file's attributes and ACL.
#include <windows.h>
#include <sddl.h>

#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "pkg/SafeFile.h"

using fw::pkg::SafeWrite;
using fw::pkg::SaveFailure;
using fw::pkg::SaveFaults;
using fw::pkg::Snapshot;

namespace {

// A fresh folder in the temp folder, removed with everything in it at the end.
class TempFolder {
 public:
  TempFolder() : path_(test::TempPath(L"safe")) { CreateDirectoryW(path_.c_str(), nullptr); }
  ~TempFolder() {
    for (const auto& f : Files()) {
      std::wstring p = path_ + L"\\" + f;
      SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(p.c_str());
    }
    RemoveDirectoryW(path_.c_str());
  }
  std::wstring operator/(const wchar_t* name) const { return path_ + L"\\" + name; }
  const std::wstring& Path() const { return path_; }
  // Every file in the folder, hidden ones included.
  std::vector<std::wstring> Files() const {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path_ + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
  }

 private:
  std::wstring path_;
};

std::vector<uint8_t> ReadAll(const std::wstring& path) {
  std::vector<uint8_t> out;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return out;
  LARGE_INTEGER size{};
  GetFileSizeEx(h, &size);
  out.resize(static_cast<size_t>(size.QuadPart));
  DWORD got = 0;
  if (!out.empty()) ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
  CloseHandle(h);
  return out;
}

std::wstring DaclOf(const std::wstring& path) {
  DWORD need = 0;
  GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &need);
  std::vector<uint8_t> sd(need);
  if (!need || !GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, sd.data(), need, &need)) return L"";
  wchar_t* text = nullptr;
  std::wstring out;
  if (ConvertSecurityDescriptorToStringSecurityDescriptorW(sd.data(), SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text,
                                                           nullptr)) {
    out = text;
    LocalFree(text);
  }
  // The entries are what matters; Windows may mark a replaced file's ACL auto-inherited ("AI").
  const size_t ace = out.find(L'(');
  if (ace != std::wstring::npos) {
    std::wstring flags = out.substr(0, ace);
    for (size_t at; (at = flags.find(L"AI")) != std::wstring::npos;) flags.erase(at, 2);
    out = flags + out.substr(ace);
  }
  return out;
}

bool SetDacl(const std::wstring& path, const wchar_t* sddl) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
  BOOL ok = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, sd);
  LocalFree(sd);
  return ok != FALSE;
}

int64_t CreationTime(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
  return static_cast<int64_t>((static_cast<uint64_t>(fa.ftCreationTime.dwHighDateTime) << 32) |
                              fa.ftCreationTime.dwLowDateTime);
}

const std::vector<uint8_t> kOriginal = test::Bytes("the original, which must survive every failure");
const std::vector<uint8_t> kNew = test::Bytes(std::string(300000, 'n') + "the new content");

}  // namespace

TEST(SafeWriteCreatesANewFile) {
  TempFolder dir;
  const std::wstring path = dir / L"new.docx";
  auto r = SafeWrite(path, kNew);
  CHECK(r.Ok());
  CHECK(ReadAll(path) == kNew);
  CHECK_EQ(dir.Files().size(), 1u);  // no temporary file left
  CHECK_EQ(r.stamp.size, kNew.size());
  CHECK(fw::pkg::CheckDisk(path, r.stamp) == Snapshot::DiskState::Unchanged);
  CHECK(fw::pkg::SameSizeAndTime(path, r.stamp));
  CHECK(!(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_HIDDEN));
}

TEST(SafeWriteKeepsAttributesAclAndCreationTime) {
  TempFolder dir;
  const std::wstring path = dir / L"report.docx";
  CHECK(test::WriteFileBytes(path, kOriginal));
  // A protected ACL unlike the folder's, and the hidden attribute.
  CHECK(SetDacl(path, L"D:P(A;;FA;;;WD)(A;;FR;;;BU)"));
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
  const std::wstring dacl = DaclOf(path);
  const int64_t created = CreationTime(path);
  Sleep(20);

  auto r = SafeWrite(path, kNew);
  CHECK(r.Ok());
  CHECK(ReadAll(path) == kNew);
  CHECK_EQ(DaclOf(path), dacl);
  CHECK_EQ(CreationTime(path), created);
  CHECK(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_HIDDEN);
  CHECK_EQ(dir.Files().size(), 1u);
  CHECK(fw::pkg::CheckDisk(path, r.stamp) == Snapshot::DiskState::Unchanged);

  // The stamp notices a change by another program, and a deleted file.
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  CHECK(test::WriteFileBytes(path, kOriginal));
  CHECK(fw::pkg::CheckDisk(path, r.stamp) == Snapshot::DiskState::Changed);
  DeleteFileW(path.c_str());
  CHECK(fw::pkg::CheckDisk(path, r.stamp) == Snapshot::DiskState::Missing);
}

TEST(SafeWriteFailuresLeaveTheOriginal) {
  struct Case {
    SaveFaults faults;
    SaveFailure expect;
  };
  const Case cases[] = {
      {{SaveFaults::Step::Create, ERROR_ACCESS_DENIED}, SaveFailure::Folder},
      {{SaveFaults::Step::Create, ERROR_DISK_FULL}, SaveFailure::DiskFull},
      {{SaveFaults::Step::Write, 0, 0}, SaveFailure::DiskFull},          // full before the first byte
      {{SaveFaults::Step::Write, 0, 70000}, SaveFailure::DiskFull},      // full partway
      {{SaveFaults::Step::Write, ERROR_IO_DEVICE}, SaveFailure::Other},  // the disk went away
      {{SaveFaults::Step::Flush, ERROR_DISK_FULL}, SaveFailure::DiskFull},
      {{SaveFaults::Step::Flush, ERROR_IO_DEVICE}, SaveFailure::Other},
      {{SaveFaults::Step::Verify}, SaveFailure::Verify},
      {{SaveFaults::Step::Replace, ERROR_SHARING_VIOLATION}, SaveFailure::InUse},
      {{SaveFaults::Step::Replace, ERROR_ACCESS_DENIED}, SaveFailure::Folder},
      {{SaveFaults::Step::ReplaceHalfway}, SaveFailure::Other},
  };
  for (const Case& c : cases) {
    TempFolder dir;
    const std::wstring path = dir / L"report.docx";
    CHECK(test::WriteFileBytes(path, kOriginal));
    auto r = SafeWrite(path, kNew, &c.faults);
    CHECK(r.failure == c.expect);
    CHECK(!r.message.empty());
    CHECK(ReadAll(path) == kOriginal);
    CHECK_EQ(dir.Files().size(), 1u);  // neither the temporary file nor the renamed original
  }
  // The same failures for a file that doesn't exist yet: nothing is left behind.
  for (const Case& c : cases) {
    if (c.faults.step == SaveFaults::Step::ReplaceHalfway) continue;
    TempFolder dir;
    auto r = SafeWrite(dir / L"new.docx", kNew, &c.faults);
    CHECK(!r.Ok());
    CHECK(dir.Files().empty());
  }
}

TEST(SafeWriteCrashBeforeReplacing) {
  TempFolder dir;
  const std::wstring path = dir / L"report.docx";
  CHECK(test::WriteFileBytes(path, kOriginal));
  SaveFaults crash{SaveFaults::Step::Crash};
  CHECK(!SafeWrite(path, kNew, &crash).Ok());
  CHECK(ReadAll(path) == kOriginal);
  // The process "died" with its temporary file written: hidden, and complete.
  const std::wstring tmp = fw::pkg::SafeWriteTempPath(path);
  CHECK(GetFileAttributesW(tmp.c_str()) & FILE_ATTRIBUTE_HIDDEN);
  CHECK(ReadAll(tmp) == kNew);
  // One left by a process that no longer exists is removed by the next save.
  const std::wstring stale = dir / L"~report.docx.4294967280.tmp";
  CHECK(test::WriteFileBytes(stale, kNew));
  CHECK(SafeWrite(path, kNew).Ok());
  CHECK(ReadAll(path) == kNew);
  CHECK_EQ(dir.Files().size(), 1u);
}

TEST(SafeWriteLockedFile) {
  TempFolder dir;
  const std::wstring path = dir / L"report.docx";
  CHECK(test::WriteFileBytes(path, kOriginal));
  {
    // Word holds the file open without sharing writes or deletes.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    auto r = SafeWrite(path, kNew);
    CHECK(r.failure == SaveFailure::InUse);
    CloseHandle(h);
    CHECK(ReadAll(path) == kOriginal);
    CHECK_EQ(dir.Files().size(), 1u);
  }
  {
    // A sync client reads it with delete sharing: the save still goes through.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    auto r = SafeWrite(path, kNew);
    CHECK(r.Ok());
    CloseHandle(h);
    CHECK(ReadAll(path) == kNew);
    CHECK_EQ(dir.Files().size(), 1u);
  }
}

TEST(SafeWriteReadOnlyFileAndFolder) {
  {
    TempFolder dir;
    const std::wstring path = dir / L"report.docx";
    CHECK(test::WriteFileBytes(path, kOriginal));
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY);
    auto r = SafeWrite(path, kNew);
    CHECK(r.failure == SaveFailure::ReadOnly);
    CHECK(ReadAll(path) == kOriginal);
    CHECK_EQ(dir.Files().size(), 1u);
  }
  {
    // A folder where new files can't be created (a share you may read and edit, say): the
    // window offers Save As elsewhere.
    TempFolder dir;
    const std::wstring path = dir / L"report.docx";
    CHECK(test::WriteFileBytes(path, kOriginal));
    CHECK(SetDacl(dir.Path(), L"D:P(D;;0x2;;;WD)(A;OICI;FA;;;WD)"));  // deny "create files" to everyone
    auto r = SafeWrite(path, kNew);
    CHECK(r.failure == SaveFailure::Folder);
    auto n = SafeWrite(dir / L"new.docx", kNew);
    CHECK(n.failure == SaveFailure::Folder);
    CHECK(SetDacl(dir.Path(), L"D:P(A;OICI;FA;;;WD)"));
    CHECK(ReadAll(path) == kOriginal);
    CHECK_EQ(dir.Files().size(), 1u);
  }
}
