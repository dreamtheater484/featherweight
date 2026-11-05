// The files around a document (plan §6.3, §13): Word's ~$ owner file, backups of originals and
// crash-recovery autosaves.
#include <windows.h>

#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "core/Strings.h"
#include "pkg/Backups.h"
#include "pkg/OwnerFile.h"
#include "pkg/Recovery.h"

namespace {

std::wstring NewFolder(const wchar_t* stem) {
  std::wstring dir = test::TempPath(stem);
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

void RemoveFolder(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      std::wstring p = dir + L"\\" + fd.cFileName;
      SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(p.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(dir.c_str());
}

size_t CountFiles(const std::wstring& dir, const wchar_t* pattern = L"*") {
  size_t n = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do n += !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  while (FindNextFileW(h, &fd));
  FindClose(h);
  return n;
}

void SetWriteTime(const std::wstring& path, int64_t t) {
  HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  FILETIME ft{static_cast<DWORD>(t), static_cast<DWORD>(static_cast<uint64_t>(t) >> 32)};
  SetFileTime(h, nullptr, nullptr, &ft);
  CloseHandle(h);
}

int64_t Now() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

constexpr int64_t kDay = 864000000000ll;

}  // namespace

// ---------------------------------------------------------------------------
// The ~$ owner file

TEST(OwnerFile_NameFollowsWordsRule) {
  using fw::pkg::OwnerFilePath;
  CHECK(OwnerFilePath(L"C:\\d\\report.docx") == L"C:\\d\\~$report.docx");      // 6: prefixed
  CHECK(OwnerFilePath(L"C:\\d\\a.docx") == L"C:\\d\\~$a.docx");
  CHECK(OwnerFilePath(L"C:\\d\\letter1.docx") == L"C:\\d\\~$etter1.docx");      // 7: first replaced
  CHECK(OwnerFilePath(L"C:\\d\\Document1.docx") == L"C:\\d\\~$cument1.docx");   // 8+: first two
  CHECK(OwnerFilePath(L"C:\\d\\Annual report.docm") == L"C:\\d\\~$nual report.docm");
}

TEST(OwnerFile_BytesAsWordWritesThem) {
  auto bytes = fw::pkg::OwnerFileBytes(L"Roy Clerx");
  CHECK_EQ(bytes.size(), 162u);
  CHECK_EQ(bytes[0], 9);
  CHECK(std::string(reinterpret_cast<const char*>(&bytes[1]), 9) == "Roy Clerx");
  CHECK_EQ(bytes[10], 0);
  CHECK_EQ(bytes[54], 9);
  CHECK_EQ(bytes[55], 0);
  CHECK_EQ(bytes[56], 'R');
  CHECK_EQ(bytes[57], 0);
  CHECK(fw::pkg::ParseOwnerFile(bytes) == L"Roy Clerx");
  // Names beyond the ANSI code page survive in the UTF-16 copy; long ones are cut to fit.
  CHECK(fw::pkg::ParseOwnerFile(fw::pkg::OwnerFileBytes(L"Zoë Ålund")) == L"Zoë Ålund");
  CHECK(fw::pkg::ParseOwnerFile(fw::pkg::OwnerFileBytes(std::wstring(80, L'x'))) == std::wstring(52, L'x'));
  // Garbage isn't a name.
  CHECK(fw::pkg::ParseOwnerFile({}).empty());
  CHECK(fw::pkg::ParseOwnerFile(std::vector<uint8_t>(10, 0)).empty());
  CHECK(fw::pkg::ParseOwnerFile(std::vector<uint8_t>{200, 'a'}).empty());
}

TEST(OwnerFile_WrittenReadAndRemovedOnlyWhenOurs) {
  std::wstring dir = NewFolder(L"owner");
  std::wstring doc = dir + L"\\Document1.docx";
  CHECK(test::WriteFileBytes(doc, test::Bytes("doc")));
  CHECK(fw::pkg::ReadOwner(doc).empty());
  CHECK(fw::pkg::WriteOwner(doc, L"Me"));
  CHECK(GetFileAttributesW((dir + L"\\~$cument1.docx").c_str()) & FILE_ATTRIBUTE_HIDDEN);
  CHECK(fw::pkg::ReadOwner(doc) == L"Me");
  CHECK(fw::pkg::WriteOwner(doc, L"Me"));  // ours again: rewritten
  fw::pkg::RemoveOwner(doc, L"Someone else");
  CHECK(fw::pkg::ReadOwner(doc) == L"Me");  // never removes another's
  fw::pkg::RemoveOwner(doc, L"Me");
  CHECK(fw::pkg::ReadOwner(doc).empty());

  // Word's: never overwritten. While Word holds the document, it is "in use by Ann".
  CHECK(fw::pkg::WriteOwner(doc, L"Ann"));
  CHECK(!fw::pkg::WriteOwner(doc, L"Me"));
  CHECK(!fw::pkg::CheckInUse(doc, L"Me").locked);  // Word crashed and left it: not in use
  HANDLE h = CreateFileW(doc.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  auto inUse = fw::pkg::CheckInUse(doc, L"Me");
  CHECK(inUse.locked);
  CHECK(inUse.user == L"Ann");
  CloseHandle(h);
  // A program reading it, sharing everything, doesn't count.
  h = CreateFileW(doc.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                  OPEN_EXISTING, 0, nullptr);
  CHECK(!fw::pkg::IsLockedByAnother(doc));
  CloseHandle(h);
  RemoveFolder(dir);
}

// ---------------------------------------------------------------------------
// Backups

TEST(Backups_KeptFoundAndNamed) {
  namespace backups = fw::pkg::backups;
  std::wstring dir = NewFolder(L"backups") + L"\\Featherweight\\Backups";  // created on demand
  const std::wstring original = L"D:\\Documents\\Annual report.docx";
  CHECK(backups::Latest(dir, original).empty());
  CHECK(!backups::Any(dir));
  std::wstring first = backups::Keep(dir, original, test::Bytes("version one"));
  CHECK(!first.empty());
  CHECK(backups::Any(dir));
  CHECK(backups::OriginalName(first) == L"Annual report.docx");
  CHECK(backups::Latest(dir, original) == first);
  CHECK(backups::Latest(dir, L"d:\\documents\\ANNUAL REPORT.docx") == first);  // paths ignore case
  CHECK(backups::Latest(dir, L"D:\\Documents\\Other.docx").empty());
  // A second backup in the same second gets its own name, and is the newest.
  std::wstring second = backups::Keep(dir, original, test::Bytes("version two"));
  CHECK(!second.empty() && second != first);
  CHECK(backups::OriginalName(second) == L"Annual report.docx");
  SetWriteTime(first, Now() - kDay);
  CHECK(backups::Latest(dir, original) == second);
  CHECK_EQ(backups::DeleteAll(dir), 2u);
  CHECK(!backups::Any(dir));
  RemoveDirectoryW(dir.c_str());
  RemoveDirectoryW(fw::DirOf(dir).c_str());
  RemoveFolder(fw::DirOf(fw::DirOf(dir)));
}

TEST(Backups_PrunedAfter30DaysAndAt1GB) {
  namespace backups = fw::pkg::backups;
  std::wstring dir = NewFolder(L"prune");
  const int64_t now = Now();
  std::vector<std::wstring> kept;
  for (int i = 0; i < 4; ++i) {
    kept.push_back(backups::Keep(dir, L"C:\\f" + std::to_wstring(i) + L".docx", std::vector<uint8_t>(1000, 'x'), now));
    CHECK(!kept.back().empty());
  }
  SetWriteTime(kept[0], now - 31 * kDay);  // too old
  SetWriteTime(kept[1], now - 20 * kDay);
  SetWriteTime(kept[2], now - 10 * kDay);
  backups::Prune(dir, now, 1ull << 30);
  CHECK_EQ(CountFiles(dir), 3u);
  CHECK(GetFileAttributesW(kept[0].c_str()) == INVALID_FILE_ATTRIBUTES);
  // Over the cap: the oldest go first.
  backups::Prune(dir, now, 2500);
  CHECK_EQ(CountFiles(dir), 2u);
  CHECK(GetFileAttributesW(kept[1].c_str()) == INVALID_FILE_ATTRIBUTES);
  CHECK(GetFileAttributesW(kept[3].c_str()) != INVALID_FILE_ATTRIBUTES);
  RemoveFolder(dir);
}

// ---------------------------------------------------------------------------
// Crash recovery

TEST(Recovery_AutosaveIsEncryptedAndReadsBack) {
  namespace recovery = fw::pkg::recovery;
  std::wstring dir = NewFolder(L"recovery");
  recovery::Session session;
  CHECK(session.Start(dir));
  std::wstring file = session.NewFile();
  const std::string secret = "PK\x03\x04 the confidential text of the document";
  CHECK(recovery::Write(file, L"D:\\Docs\\Plan.docx", L"Plan.docx", test::Bytes(secret), 12345));
  // Rewritten in place: the newest content wins.
  CHECK(recovery::Write(file, L"D:\\Docs\\Plan.docx", L"Plan.docx", test::Bytes(secret + " v2"), 12346));
  std::vector<uint8_t> raw;
  {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    raw.resize(4096);
    DWORD got = 0;
    ReadFile(h, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr);
    raw.resize(got);
    CloseHandle(h);
  }
  CHECK(test::Text(raw).find("confidential") == std::string::npos);  // never in clear
  CHECK(test::Text(raw).find("Plan") == std::string::npos);

  recovery::Entry e;
  std::vector<uint8_t> doc;
  CHECK(recovery::Read(file, e, doc));
  CHECK(test::Text(doc) == secret + " v2");
  CHECK(e.originalPath == L"D:\\Docs\\Plan.docx");
  CHECK(e.title == L"Plan.docx");
  CHECK_EQ(e.savedAt, 12346);
  // A damaged file isn't read.
  raw[raw.size() / 2] ^= 0x55;
  std::wstring damaged = dir + L"\\damaged.fwr";
  CHECK(test::WriteFileBytes(damaged, raw));
  CHECK(!recovery::Read(damaged, e, doc));
  raw.resize(6);
  CHECK(test::WriteFileBytes(damaged, raw));
  CHECK(!recovery::Read(damaged, e, doc));
  session.End();
  RemoveFolder(dir);
}

TEST(Recovery_OnlyOrphansAreOffered) {
  namespace recovery = fw::pkg::recovery;
  std::wstring dir = NewFolder(L"orphans");
  // A running session's autosaves aren't offered to another one.
  recovery::Session running;
  CHECK(running.Start(dir));
  CHECK(recovery::Write(running.NewFile(), L"C:\\a.docx", L"a.docx", test::Bytes("a")));
  CHECK(recovery::FindOrphans(dir).empty());
  // A session that crashed: its lock went with its process (here: a lock nobody holds, as after
  // a power cut, and one that is gone).
  CHECK(test::WriteFileBytes(dir + L"\\session-1-1.lock", {}));
  CHECK(recovery::Write(dir + L"\\session-1-1-1.fwr", L"C:\\b.docx", L"b.docx", test::Bytes("b"), 200));
  CHECK(recovery::Write(dir + L"\\session-2-2-1.fwr", L"", L"Document 1", test::Bytes("c"), 100));
  auto orphans = recovery::FindOrphans(dir);
  CHECK_EQ(orphans.size(), 2u);
  if (orphans.size() == 2) {
    CHECK(orphans[0].title == L"Document 1");  // oldest first
    CHECK(orphans[0].originalPath.empty());
    CHECK(orphans[1].originalPath == L"C:\\b.docx");
  }
  CHECK(GetFileAttributesW((dir + L"\\session-1-1.lock").c_str()) == INVALID_FILE_ATTRIBUTES);
  for (const auto& o : orphans) recovery::Remove(o.file);
  CHECK(recovery::FindOrphans(dir).empty());
  // A clean exit removes the session's own autosaves and its lock.
  running.End();
  CHECK_EQ(CountFiles(dir), 0u);
  RemoveFolder(dir);
}
