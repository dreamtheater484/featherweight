#include "pkg/Backups.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#include "core/Strings.h"
#include "pkg/SafeFile.h"

namespace fw::pkg::backups {
namespace {

constexpr int64_t kTicksPerDay = 864000000000ll;

int64_t Now() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

int64_t Ticks(const FILETIME& ft) {
  return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

// A short hash of the original's full path (FNV-1a, case-insensitive), so its backups can be found.
std::wstring Tag(const std::wstring& path) {
  uint32_t h = 2166136261u;
  for (wchar_t c : path) {
    h ^= static_cast<uint32_t>(std::towlower(c));
    h *= 16777619u;
  }
  wchar_t buf[9];
  swprintf_s(buf, L"%08x", h);
  return buf;
}

void CreateFolder(const std::wstring& dir) {
  if (dir.empty() || CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return;
  CreateFolder(DirOf(dir));
  CreateDirectoryW(dir.c_str(), nullptr);
}

struct Found {
  std::wstring name;
  int64_t time;
  uint64_t size;
};

std::vector<Found> List(const std::wstring& dir, const std::wstring& pattern = L"*") {
  std::vector<Found> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    out.push_back({fd.cFileName, Ticks(fd.ftLastWriteTime),
                   (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

void Remove(const std::wstring& path) {
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  DeleteFileW(path.c_str());
}

}  // namespace

std::wstring Keep(const std::wstring& dir, const std::wstring& originalPath, std::span<const uint8_t> bytes,
                  int64_t now) {
  if (dir.empty()) return {};
  if (!now) now = Now();
  CreateFolder(dir);
  const std::wstring name = FileNameOf(originalPath);
  const size_t dot = name.rfind(L'.');
  const std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);
  const std::wstring ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot);
  FILETIME ft{static_cast<DWORD>(now), static_cast<DWORD>(static_cast<uint64_t>(now) >> 32)}, local;
  SYSTEMTIME st{};
  FileTimeToLocalFileTime(&ft, &local);
  FileTimeToSystemTime(&local, &st);
  wchar_t when[32];
  swprintf_s(when, L" %04u-%02u-%02u %02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  std::wstring path = dir + L"\\" + stem + when + L" #" + Tag(originalPath) + ext;
  for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; ++n)
    path = dir + L"\\" + stem + when + L"-" + std::to_wstring(n) + L" #" + Tag(originalPath) + ext;
  if (!SafeWrite(path, bytes).Ok()) return {};
  Prune(dir, now);
  return path;
}

std::wstring Latest(const std::wstring& dir, const std::wstring& originalPath) {
  if (dir.empty()) return {};
  std::vector<Found> found = List(dir, L"* #" + Tag(originalPath) + L".*");
  // A file without an extension has none after the tag either.
  for (Found& f : List(dir, L"* #" + Tag(originalPath))) found.push_back(std::move(f));
  if (found.empty()) return {};
  auto newest = std::max_element(found.begin(), found.end(), [](const Found& a, const Found& b) {
    return a.time != b.time ? a.time < b.time : a.name < b.name;
  });
  return dir + L"\\" + newest->name;
}

std::wstring OriginalName(const std::wstring& backupPath) {
  std::wstring name = FileNameOf(backupPath);
  const size_t tag = name.rfind(L" #");
  if (tag == std::wstring::npos || tag < 18) return name;
  const size_t dot = name.find(L'.', tag);
  const std::wstring ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot);
  std::wstring stem = name.substr(0, tag);
  const size_t when = stem.rfind(L' ', stem.rfind(L' ') - 1);  // " yyyy-mm-dd hhmmss[-n]"
  if (when != std::wstring::npos) stem.erase(when);
  return stem + ext;
}

void Prune(const std::wstring& dir, int64_t now, uint64_t maxBytes, int days) {
  if (dir.empty()) return;
  if (!now) now = Now();
  std::vector<Found> found = List(dir);
  std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.time < b.time; });
  uint64_t total = 0;
  for (const Found& f : found) total += f.size;
  for (const Found& f : found) {
    if (now - f.time <= days * kTicksPerDay && total <= maxBytes) break;
    Remove(dir + L"\\" + f.name);
    total -= f.size;
  }
}

size_t DeleteAll(const std::wstring& dir) {
  if (dir.empty()) return 0;
  std::vector<Found> found = List(dir);
  for (const Found& f : found) Remove(dir + L"\\" + f.name);
  return found.size();
}

bool Any(const std::wstring& dir) { return !dir.empty() && !List(dir).empty(); }

}  // namespace fw::pkg::backups
