#include "pkg/Snapshot.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "core/Strings.h"

namespace fw::pkg {
namespace {

constexpr size_t kChunk = 4u << 20;

// Incremental SHA-256 through CNG.
class Hasher {
 public:
  Hasher() { BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash_, nullptr, 0, nullptr, 0, 0); }
  ~Hasher() {
    if (hash_) BCryptDestroyHash(hash_);
  }
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;

  void Add(const void* data, size_t size) {
    if (!hash_) return;
    auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
      ULONG part = static_cast<ULONG>(std::min<size_t>(size, 1u << 30));
      BCryptHashData(hash_, const_cast<PUCHAR>(p), part, 0);
      p += part;
      size -= part;
    }
  }

  Sha256 Finish() {
    Sha256 out{};
    if (hash_) BCryptFinishHash(hash_, out.data(), static_cast<ULONG>(out.size()), 0);
    return out;
  }

 private:
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

struct FileInfo {
  uint64_t size = 0;
  int64_t lastWrite = 0;
};

bool QueryInfo(HANDLE h, FileInfo& info) {
  BY_HANDLE_FILE_INFORMATION fi{};
  if (!GetFileInformationByHandle(h, &fi)) return false;
  info.size = (static_cast<uint64_t>(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
  info.lastWrite = static_cast<int64_t>((static_cast<uint64_t>(fi.ftLastWriteTime.dwHighDateTime) << 32) |
                                        fi.ftLastWriteTime.dwLowDateTime);
  return true;
}

// Opened so that nobody else is locked out: sync clients and Office keep working on the file.
HANDLE OpenShared(const std::wstring& path) {
  return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                     OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

bool ReadExact(HANDLE h, void* dst, size_t size) {
  auto* p = static_cast<uint8_t*>(dst);
  while (size > 0) {
    DWORD got = 0;
    if (!ReadFile(h, p, static_cast<DWORD>(std::min(size, kChunk)), &got, nullptr) || got == 0) return false;
    p += got;
    size -= got;
  }
  return true;
}

bool WriteExact(HANDLE h, const void* src, size_t size) {
  auto* p = static_cast<const uint8_t*>(src);
  while (size > 0) {
    DWORD put = 0;
    if (!WriteFile(h, p, static_cast<DWORD>(std::min(size, kChunk)), &put, nullptr) || put == 0) return false;
    p += put;
    size -= put;
  }
  return true;
}

std::wstring NewCopyPath(const std::wstring& dir) {
  static std::atomic<uint32_t> counter{0};
  std::wstring base = dir;
  if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';
  return base + L"snapshot-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter) +
         L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
}

}  // namespace

Sha256 HashBytes(std::span<const uint8_t> bytes) {
  Hasher hasher;
  hasher.Add(bytes.data(), bytes.size());
  return hasher.Finish();
}

Snapshot::~Snapshot() {
  if (copy_) CloseHandle(copy_);
}

std::unique_ptr<Snapshot> Snapshot::FromBytes(std::vector<uint8_t> bytes) {
  std::unique_ptr<Snapshot> snap(new Snapshot());
  snap->stamp_.size = bytes.size();
  snap->stamp_.hash = HashBytes(bytes);
  snap->bytes_ = std::move(bytes);
  return snap;
}

std::unique_ptr<Snapshot> Snapshot::Open(const std::wstring& path, const Options& options, Status& status) {
  const std::string name = WideToUtf8(FileNameOf(path));
  // A file that changes while it is being read gets one more try, then a plain message.
  for (int attempt = 0; attempt < 2; ++attempt) {
    HANDLE h = OpenShared(path);
    if (h == INVALID_HANDLE_VALUE) {
      DWORD err = GetLastError();
      status = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND
                   ? Status::Fail(Status::Code::NotFound, name + " doesn't exist anymore.")
                   : Status::Fail(Status::Code::IoError, name + " couldn't be opened. Another program may be using it.");
      return nullptr;
    }
    FileInfo before;
    if (!QueryInfo(h, before)) {
      CloseHandle(h);
      status = Status::Fail(Status::Code::IoError, name + " couldn't be read.");
      return nullptr;
    }

    std::unique_ptr<Snapshot> snap(new Snapshot());
    snap->path_ = path;
    Hasher hasher;
    bool read = false;
    if (before.size <= options.memoryLimit) {
      snap->bytes_.resize(static_cast<size_t>(before.size));
      read = ReadExact(h, snap->bytes_.data(), snap->bytes_.size());
      if (read) hasher.Add(snap->bytes_.data(), snap->bytes_.size());
    } else {
      if (options.recoveryDir.empty()) {
        CloseHandle(h);
        status = Status::Fail(Status::Code::IoError, "There's no recovery folder to keep a copy of " + name + ".");
        return nullptr;
      }
      CreateDirectoryW(options.recoveryDir.c_str(), nullptr);
      HANDLE copy = CreateFileW(NewCopyPath(options.recoveryDir).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                FILE_SHARE_READ, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
      if (copy == INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        status = Status::Fail(Status::Code::IoError, "A private copy of " + name + " couldn't be made.");
        return nullptr;
      }
      snap->copy_ = copy;
      std::vector<uint8_t> buffer(kChunk);
      uint64_t left = before.size;
      read = true;
      while (read && left > 0) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
        read = ReadExact(h, buffer.data(), n);
        if (read && !WriteExact(copy, buffer.data(), n)) {
          CloseHandle(h);
          status = Status::Fail(Status::Code::IoError,
                                "There isn't enough space to keep a private copy of " + name + ".");
          return nullptr;
        }
        hasher.Add(buffer.data(), n);
        left -= n;
      }
    }
    FileInfo after;
    bool same = QueryInfo(h, after) && after.size == before.size && after.lastWrite == before.lastWrite;
    CloseHandle(h);
    if (read && same) {
      snap->stamp_ = {before.size, before.lastWrite, hasher.Finish()};
      status = Status{};
      return snap;
    }
  }
  status = Status::Fail(Status::Code::Changed, name + " changed while it was being opened. Try again in a moment.");
  return nullptr;
}

bool Snapshot::ReadAt(uint64_t offset, void* dst, size_t size) const {
  if (offset > stamp_.size || size > stamp_.size - offset) return false;
  if (!copy_) {
    if (size) std::memcpy(dst, bytes_.data() + offset, size);
    return true;
  }
  auto* p = static_cast<uint8_t*>(dst);
  while (size > 0) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(copy_, p, static_cast<DWORD>(std::min(size, kChunk)), &got, &ov) || got == 0) return false;
    p += got;
    offset += got;
    size -= got;
  }
  return true;
}

Snapshot::DiskState Snapshot::CheckDisk() const {
  if (path_.empty()) return DiskState::Unchanged;
  return pkg::CheckDisk(path_, stamp_);
}

Snapshot::DiskState CheckDisk(const std::wstring& path, const Snapshot::Stamp& stamp) {
  HANDLE h = OpenShared(path);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? Snapshot::DiskState::Missing
                                                                       : Snapshot::DiskState::Changed;
  }
  FileInfo now;
  Snapshot::DiskState state = Snapshot::DiskState::Changed;
  if (QueryInfo(h, now) && now.size == stamp.size && now.lastWrite == stamp.lastWrite) {
    // Same size and time. Some tools keep the time when they write, so the hash decides.
    Hasher hasher;
    std::vector<uint8_t> buffer(static_cast<size_t>(std::min<uint64_t>(now.size, kChunk)));
    uint64_t left = now.size;
    bool read = true;
    while (read && left > 0) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
      read = ReadExact(h, buffer.data(), n);
      hasher.Add(buffer.data(), n);
      left -= n;
    }
    if (read && hasher.Finish() == stamp.hash) state = Snapshot::DiskState::Unchanged;
  }
  CloseHandle(h);
  return state;
}

bool SameSizeAndTime(const std::wstring& path, const Snapshot::Stamp& stamp) {
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
  const uint64_t size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
  const int64_t time = static_cast<int64_t>((static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
                                            fa.ftLastWriteTime.dwLowDateTime);
  return size == stamp.size && time == stamp.lastWrite;
}

bool StampOf(const std::wstring& path, const Sha256& hash, Snapshot::Stamp& stamp) {
  HANDLE h = OpenShared(path);
  if (h == INVALID_HANDLE_VALUE) return false;
  FileInfo info;
  bool ok = QueryInfo(h, info);
  CloseHandle(h);
  if (ok) stamp = {info.size, info.lastWrite, hash};
  return ok;
}

}  // namespace fw::pkg
