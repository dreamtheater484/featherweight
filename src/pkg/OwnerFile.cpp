#include "pkg/OwnerFile.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/Strings.h"

namespace fw::pkg {
namespace {

constexpr size_t kSize = 162;
constexpr size_t kUnicodeAt = 54;  // the UTF-16 name's length (2 bytes), then the name
constexpr size_t kMaxName = 52;    // characters, so both copies keep a terminating zero

bool ReadSmallFile(const std::wstring& path, std::vector<uint8_t>& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out.resize(1024);
  DWORD got = 0;
  bool ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) != FALSE;
  CloseHandle(h);
  out.resize(ok ? got : 0);
  return ok;
}

}  // namespace

std::wstring OwnerFilePath(const std::wstring& docPath) {
  const std::wstring dir = DirOf(docPath);
  std::wstring name = FileNameOf(docPath);
  const size_t dot = name.rfind(L'.');
  const size_t stem = dot == std::wstring::npos ? name.size() : dot;
  if (stem >= 8) name.erase(0, 2);
  else if (stem == 7) name.erase(0, 1);
  return (dir.empty() ? std::wstring() : dir + L"\\") + L"~$" + name;
}

std::vector<uint8_t> OwnerFileBytes(const std::wstring& user) {
  std::vector<uint8_t> out(kSize, 0);
  const std::wstring name = user.substr(0, kMaxName);
  char ansi[kMaxName * 2 + 1] = {};
  int n = WideCharToMultiByte(CP_ACP, 0, name.data(), static_cast<int>(name.size()), ansi, sizeof(ansi) - 1, "?",
                              nullptr);
  n = std::clamp(n, 0, static_cast<int>(kMaxName));
  out[0] = static_cast<uint8_t>(n);
  std::memcpy(&out[1], ansi, static_cast<size_t>(n));
  out[kUnicodeAt] = static_cast<uint8_t>(name.size());
  out[kUnicodeAt + 1] = 0;
  for (size_t i = 0; i < name.size(); ++i) {
    out[kUnicodeAt + 2 + i * 2] = static_cast<uint8_t>(name[i] & 0xFF);
    out[kUnicodeAt + 3 + i * 2] = static_cast<uint8_t>(name[i] >> 8);
  }
  return out;
}

std::wstring ParseOwnerFile(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return {};
  // The UTF-16 copy, when it is there.
  if (bytes.size() >= kUnicodeAt + 2) {
    size_t len = bytes[kUnicodeAt] | (bytes[kUnicodeAt + 1] << 8);
    if (len > 0 && len <= kMaxName + 1 && kUnicodeAt + 2 + len * 2 <= bytes.size()) {
      std::wstring name(len, L'\0');
      for (size_t i = 0; i < len; ++i)
        name[i] = static_cast<wchar_t>(bytes[kUnicodeAt + 2 + i * 2] | (bytes[kUnicodeAt + 3 + i * 2] << 8));
      if (name.find(L'\0') == std::wstring::npos) return name;
    }
  }
  size_t len = bytes[0];
  if (len == 0 || 1 + len > bytes.size() || len > kUnicodeAt - 1) return {};
  int n = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(&bytes[1]), static_cast<int>(len), nullptr, 0);
  std::wstring name(static_cast<size_t>(std::max(n, 0)), L'\0');
  if (n > 0)
    MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(&bytes[1]), static_cast<int>(len), name.data(), n);
  if (name.find(L'\0') != std::wstring::npos) return {};
  return name;
}

std::wstring ReadOwner(const std::wstring& docPath) {
  std::vector<uint8_t> bytes;
  if (!ReadSmallFile(OwnerFilePath(docPath), bytes)) return {};
  return ParseOwnerFile(bytes);
}

bool WriteOwner(const std::wstring& docPath, const std::wstring& user) {
  const std::wstring path = OwnerFilePath(docPath);
  const std::wstring current = ReadOwner(docPath);
  if (!current.empty() && current != user) return false;  // someone else's
  const std::vector<uint8_t> bytes = OwnerFileBytes(user);
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_HIDDEN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size();
  CloseHandle(h);
  if (!ok) DeleteFileW(path.c_str());
  return ok;
}

void RemoveOwner(const std::wstring& docPath, const std::wstring& user) {
  if (ReadOwner(docPath) != user) return;
  const std::wstring path = OwnerFilePath(docPath);
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  if (!DeleteFileW(path.c_str())) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
}

bool IsLockedByAnother(const std::wstring& path) {
  // Reading while refusing to share writes fails when anyone has it open for writing, and it
  // needs no write permission, so read-only files and shares work too.
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
    return false;
  }
  DWORD e = GetLastError();
  return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
}

InUse CheckInUse(const std::wstring& docPath, const std::wstring& self) {
  InUse r;
  r.locked = IsLockedByAnother(docPath);
  if (r.locked) {
    r.user = ReadOwner(docPath);
    if (r.user == self) r.user.clear();
  }
  return r;
}

}  // namespace fw::pkg
