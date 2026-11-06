#include "pkg/SafeFile.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/Strings.h"

namespace fw::pkg {
namespace {

constexpr size_t kChunk = 1u << 20;

bool Inject(const SaveFaults* faults, SaveFaults::Step step) {
  if (!faults || faults->step != step) return false;
  SetLastError(faults->error ? faults->error : ERROR_GEN_FAILURE);
  return true;
}

// Replaces `target` with `source` even while another handle (opened with FILE_SHARE_DELETE) has
// it open, which ReplaceFileW refuses. Used only when ReplaceFileW fails.
bool PosixReplace(const std::wstring& source, const std::wstring& target) {
  HANDLE h = CreateFileW(source.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  std::vector<unsigned char> buf(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t), 0);
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
  info->Flags = 0x1 /*FILE_RENAME_FLAG_REPLACE_IF_EXISTS*/ | 0x2 /*FILE_RENAME_FLAG_POSIX_SEMANTICS*/;
  info->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
  std::memcpy(info->FileName, target.c_str(), info->FileNameLength);
  BOOL ok = SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(22) /*FileRenameInfoEx*/, info,
                                       static_cast<DWORD>(buf.size()));
  DWORD err = GetLastError();
  CloseHandle(h);
  SetLastError(err);
  return ok != FALSE;
}

// Temporary files a crashed Featherweight left beside `path` (theirs is `~name.<pid>.tmp`).
void RemoveStaleTemps(const std::wstring& path) {
  const std::wstring dir = DirOf(path);
  const std::wstring prefix = L"~" + FileNameOf(path) + L".";
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((dir + L"\\" + prefix + L"*.tmp").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    std::wstring name = fd.cFileName;
    if (name.size() <= prefix.size() + 4) continue;
    std::wstring pid = name.substr(prefix.size(), name.size() - prefix.size() - 4);
    if (pid.empty() || pid.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
    DWORD id = static_cast<DWORD>(std::wcstoul(pid.c_str(), nullptr, 10));
    if (id == GetCurrentProcessId()) continue;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id)) {
      CloseHandle(p);  // still running: maybe saving right now
      continue;
    }
    if (GetLastError() != ERROR_INVALID_PARAMETER) continue;  // exists, but we may not look
    std::wstring stale = dir + L"\\" + name;
    SetFileAttributesW(stale.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(stale.c_str());
  } while (FindNextFileW(find, &fd));
  FindClose(find);
}

// Step::Write writes `writeLimit` bytes (or all of them), then fails as a full disk would.
bool WriteAll(HANDLE h, std::span<const uint8_t> bytes, const SaveFaults* faults) {
  const bool inject = faults && faults->step == SaveFaults::Step::Write;
  const size_t end = inject ? static_cast<size_t>(std::min<uint64_t>(faults->writeLimit, bytes.size())) : bytes.size();
  size_t done = 0;
  while (done < end) {
    DWORD put = 0;
    if (!WriteFile(h, bytes.data() + done, static_cast<DWORD>(std::min(end - done, kChunk)), &put, nullptr) || put == 0)
      return false;
    done += put;
  }
  if (inject) SetLastError(faults->error ? faults->error : ERROR_DISK_FULL);
  return !inject;
}

// Reads the file back through a new handle and compares it with what was meant to be written.
bool ReadsBackSame(const std::wstring& path, std::span<const uint8_t> bytes) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool same = GetFileSizeEx(h, &size) && static_cast<uint64_t>(size.QuadPart) == bytes.size();
  std::vector<uint8_t> buffer(std::min(bytes.size(), kChunk));
  size_t done = 0;
  while (same && done < bytes.size()) {
    DWORD got = 0;
    DWORD want = static_cast<DWORD>(std::min(bytes.size() - done, buffer.size()));
    same = ReadFile(h, buffer.data(), want, &got, nullptr) && got > 0 &&
           std::memcmp(buffer.data(), bytes.data() + done, got) == 0;
    done += got;
  }
  CloseHandle(h);
  return same;
}

bool IsDiskFull(DWORD e) { return e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL || e == ERROR_DISK_QUOTA_EXCEEDED; }
bool IsInUse(DWORD e) {
  return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_USER_MAPPED_FILE ||
         e == ERROR_UNABLE_TO_REMOVE_REPLACED || e == ERROR_UNABLE_TO_MOVE_REPLACEMENT;
}
bool IsDenied(DWORD e) { return e == ERROR_ACCESS_DENIED || e == ERROR_WRITE_PROTECT || e == ERROR_NETWORK_ACCESS_DENIED; }

SaveResult Fail(SaveFailure failure, std::string message) {
  SaveResult r;
  r.failure = failure;
  r.message = std::move(message);
  return r;
}

}  // namespace

std::string SystemMessage(uint32_t error) {
  wchar_t* text = nullptr;
  DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  std::wstring msg = n && text ? std::wstring(text, n) : std::wstring();
  if (text) LocalFree(text);
  while (!msg.empty() && (msg.back() == L'\n' || msg.back() == L'\r' || msg.back() == L' ' || msg.back() == L'.'))
    msg.pop_back();
  return WideToUtf8(msg);
}

std::wstring SafeWriteTempPath(const std::wstring& path) {
  return DirOf(path) + L"\\~" + FileNameOf(path) + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
}

SaveResult SafeWrite(const std::wstring& path, std::span<const uint8_t> bytes, const SaveFaults* faults) {
  const std::string name = WideToUtf8(FileNameOf(path));
  const std::string unchanged = name + " wasn't changed.";
  const DWORD oldAttrs = GetFileAttributesW(path.c_str());
  const bool exists = oldAttrs != INVALID_FILE_ATTRIBUTES && !(oldAttrs & FILE_ATTRIBUTE_DIRECTORY);
  if (exists && (oldAttrs & FILE_ATTRIBUTE_READONLY))
    return Fail(SaveFailure::ReadOnly, name + " is read-only, so it can't be replaced.");

  RemoveStaleTemps(path);
  const std::wstring tmp = SafeWriteTempPath(path);
  SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);  // a leftover of ours, if any, is overwritten
  HANDLE h = Inject(faults, SaveFaults::Step::Create)
                 ? INVALID_HANDLE_VALUE
                 : CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    if (IsDiskFull(e)) return Fail(SaveFailure::DiskFull, "There isn't enough space on the disk to save " + name + ".");
    if (IsDenied(e)) return Fail(SaveFailure::Folder, "Featherweight can't save in this folder.");
    return Fail(SaveFailure::Other, name + " couldn't be saved: " + SystemMessage(e) + ".");
  }

  auto abandon = [&](SaveFailure failure, std::string message) {
    DWORD e = GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(tmp.c_str());
    SetLastError(e);
    return Fail(failure, std::move(message));
  };

  if (!WriteAll(h, bytes, faults)) {
    DWORD e = GetLastError();
    if (IsDiskFull(e))
      return abandon(SaveFailure::DiskFull, "There isn't enough space on the disk to save " + name + ". " + unchanged);
    return abandon(SaveFailure::Other, name + " couldn't be written: " + SystemMessage(e) + ". " + unchanged);
  }
  if (Inject(faults, SaveFaults::Step::Flush) || !FlushFileBuffers(h)) {
    DWORD e = GetLastError();
    if (IsDiskFull(e))
      return abandon(SaveFailure::DiskFull, "There isn't enough space on the disk to save " + name + ". " + unchanged);
    return abandon(SaveFailure::Other, name + " couldn't be written to the disk. " + unchanged);
  }
  CloseHandle(h);
  h = INVALID_HANDLE_VALUE;

  if (Inject(faults, SaveFaults::Step::Verify) || !ReadsBackSame(tmp, bytes))
    return abandon(SaveFailure::Verify, name + " couldn't be saved: the copy on the disk didn't read back correctly. " +
                                            unchanged);

  // A crash here leaves the original as it was and the hidden temporary file, which the next save removes.
  if (faults && faults->step == SaveFaults::Step::Crash) return Fail(SaveFailure::Other, "Stopped before replacing.");

  // ReplaceFileW is given a backup name in the same folder: without one, a failure halfway can
  // leave the original deleted (ERROR_UNABLE_TO_MOVE_REPLACEMENT).
  const std::wstring old = tmp.substr(0, tmp.size() - 4) + L".old";
  bool replaced = false;
  if (exists) {
    SetFileAttributesW(old.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(old.c_str());
    if (Inject(faults, SaveFaults::Step::Replace)) {
    } else if (faults && faults->step == SaveFaults::Step::ReplaceHalfway) {
      MoveFileExW(path.c_str(), old.c_str(), 0);  // as if the replacement couldn't be moved in
      SetLastError(ERROR_UNABLE_TO_MOVE_REPLACEMENT_2);
    } else {
      replaced = ReplaceFileW(path.c_str(), tmp.c_str(), old.c_str(),
                              REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr) != FALSE;
    }
    if (replaced) {
      SetFileAttributesW(old.c_str(), FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(old.c_str());
    } else {
      DWORD e = GetLastError();
      // The original was renamed but the new file couldn't take its place: put the original back.
      if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES && !MoveFileExW(old.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return Fail(SaveFailure::Other, name + " couldn't be replaced, and the original is now named " +
                                            WideToUtf8(FileNameOf(old)) + " in the same folder.");
      }
      // Another program may hold the file with delete sharing (sync clients do), which ReplaceFileW
      // refuses. The new file is whole, so swap it in by name, with the original's attributes. Not
      // for a file we may not write: that would drop its ACL.
      const bool shared = e == ERROR_SHARING_VIOLATION || e == ERROR_UNABLE_TO_REMOVE_REPLACED ||
                          e == ERROR_UNABLE_TO_MOVE_REPLACEMENT;
      if (shared && !(faults && faults->step != SaveFaults::Step::None)) {
        SetFileAttributesW(tmp.c_str(), oldAttrs | FILE_ATTRIBUTE_ARCHIVE);
        replaced = PosixReplace(tmp, path);
        if (!replaced) SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_HIDDEN);
      }
      if (!replaced) SetLastError(e);
    }
  } else if (Inject(faults, SaveFaults::Step::Replace)) {
  } else {
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_ARCHIVE);
    replaced = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
  }
  if (!replaced) {
    DWORD e = GetLastError();
    if (IsInUse(e))
      return abandon(SaveFailure::InUse, "Another program is using " + name + ", so it couldn't be replaced. " + unchanged);
    if (IsDenied(e)) return abandon(SaveFailure::Folder, "Featherweight can't save in this folder. " + unchanged);
    if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS)
      return abandon(SaveFailure::InUse, "Another program created " + name + " while it was being saved.");
    return abandon(SaveFailure::Other, name + " couldn't be replaced: " + SystemMessage(e) + ". " + unchanged);
  }
  // ReplaceFileW keeps the original's ACLs, creation time and streams; its attributes (hidden,
  // say) are put back here, since the temporary file was hidden.
  if (exists) SetFileAttributesW(path.c_str(), (oldAttrs & ~FILE_ATTRIBUTE_NORMAL) | FILE_ATTRIBUTE_ARCHIVE);

  SaveResult r;
  if (!StampOf(path, HashBytes(bytes), r.stamp)) r.stamp = {bytes.size(), 0, HashBytes(bytes)};
  return r;
}

}  // namespace fw::pkg
