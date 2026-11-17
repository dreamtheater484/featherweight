#include "pkg/Recovery.h"

#include <windows.h>
#include <dpapi.h>

#include <algorithm>
#include <cstring>

#include "core/Strings.h"

namespace fw::pkg::recovery {
namespace {

constexpr char kFileMagic[4] = {'F', 'W', 'R', 'C'};     // the file: magic, version, DPAPI blob
constexpr char kPayloadMagic[4] = {'F', 'W', 'R', 'P'};  // inside the blob
constexpr uint32_t kWholeVersion = 1;  // a whole autosave; journals are version 2

int64_t Now() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

void CreateFolder(const std::wstring& dir) {
  if (dir.empty() || CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return;
  CreateFolder(DirOf(dir));
  CreateDirectoryW(dir.c_str(), nullptr);
}

template <typename T>
void Put(std::vector<uint8_t>& out, T v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}

void PutString(std::vector<uint8_t>& out, const std::wstring& s) {
  Put<uint32_t>(out, static_cast<uint32_t>(s.size()));
  const auto* p = reinterpret_cast<const uint8_t*>(s.data());
  out.insert(out.end(), p, p + s.size() * sizeof(wchar_t));
}

// Reads from a payload, refusing to go past its end.
struct Reader {
  std::span<const uint8_t> data;
  size_t at = 0;
  bool ok = true;

  template <typename T>
  T Get() {
    T v{};
    if (!ok || data.size() - at < sizeof(T)) {
      ok = false;
      return v;
    }
    std::memcpy(&v, data.data() + at, sizeof(T));
    at += sizeof(T);
    return v;
  }
  std::wstring GetString() {
    uint32_t n = Get<uint32_t>();
    if (!ok || n > 32768 || (data.size() - at) / sizeof(wchar_t) < n) {
      ok = false;
      return {};
    }
    std::wstring s(n, L'\0');
    std::memcpy(s.data(), data.data() + at, n * sizeof(wchar_t));
    at += n * sizeof(wchar_t);
    return s;
  }
};

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
  if (ok) {
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (ok && done < out.size()) {
      DWORD got = 0;
      ok = ReadFile(h, out.data() + done, static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 24)), &got,
                    nullptr) && got > 0;
      done += got;
    }
  }
  CloseHandle(h);
  return ok;
}

// The session a file belongs to: "session-<pid>-<ticks>" from "session-<pid>-<ticks>-<n>.fwr".
std::wstring SessionOf(const std::wstring& name) {
  const size_t dash = name.rfind(L'-');
  return dash == std::wstring::npos ? std::wstring() : name.substr(0, dash);
}

}  // namespace

Session::~Session() { End(); }

bool Session::Start(const std::wstring& dir) {
  End();
  if (dir.empty()) return false;
  CreateFolder(dir);
  dir_ = dir;
  prefix_ = L"session-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  HANDLE h = CreateFileW((dir_ + L"\\" + prefix_ + L".lock").c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  lock_ = h == INVALID_HANDLE_VALUE ? nullptr : h;
  return lock_ != nullptr;
}

std::wstring Session::NewFile() {
  if (!lock_) return {};
  return dir_ + L"\\" + prefix_ + L"-" + std::to_wstring(++next_) + L".fwr";
}

void Session::End() {
  if (!lock_) return;
  CloseHandle(lock_);  // deletes the lock file
  lock_ = nullptr;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((dir_ + L"\\" + prefix_ + L"-*.fwr").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) return;
  do Remove(dir_ + L"\\" + fd.cFileName);
  while (FindNextFileW(find, &fd));
  FindClose(find);
}

bool Write(const std::wstring& file, const std::wstring& originalPath, const std::wstring& title,
           std::span<const uint8_t> document, int64_t now) {
  if (file.empty()) return false;
  std::vector<uint8_t> payload(kPayloadMagic, kPayloadMagic + 4);
  Put<uint32_t>(payload, kWholeVersion);
  Put<int64_t>(payload, now ? now : Now());
  PutString(payload, originalPath);
  PutString(payload, title);
  Put<uint64_t>(payload, document.size());
  payload.insert(payload.end(), document.begin(), document.end());

  DATA_BLOB in{static_cast<DWORD>(payload.size()), payload.data()}, out{};
  if (!CryptProtectData(&in, L"Featherweight recovery", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
    return false;
  std::vector<uint8_t> bytes(kFileMagic, kFileMagic + 4);
  Put<uint32_t>(bytes, kWholeVersion);
  bytes.insert(bytes.end(), out.pbData, out.pbData + out.cbData);
  SecureZeroMemory(payload.data(), payload.size());
  LocalFree(out.pbData);

  const std::wstring tmp = file + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size() &&
            FlushFileBuffers(h);
  CloseHandle(h);
  ok = ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

namespace {

// The document's payload (version 1's whole blob, version 2's first piece).
bool ReadDocument(std::span<const uint8_t> plain, uint32_t version, Entry& entry, std::vector<uint8_t>& document) {
  Reader r{plain};
  if (plain.size() < 4 || std::memcmp(plain.data(), kPayloadMagic, 4) != 0) return false;
  r.at = 4;
  r.Get<uint32_t>();  // the payload's version
  entry.savedAt = r.Get<int64_t>();
  entry.originalPath = r.GetString();
  entry.title = r.GetString();
  if (version >= 2) {
    uint32_t n = r.Get<uint32_t>();
    if (!r.ok || n > 65536 || plain.size() - r.at < n) return false;
    entry.meta.assign(reinterpret_cast<const char*>(plain.data() + r.at), n);
    r.at += n;
  }
  uint64_t size = r.Get<uint64_t>();
  if (!r.ok || size != plain.size() - r.at) return false;
  document.assign(plain.begin() + static_cast<ptrdiff_t>(r.at), plain.end());
  return true;
}

// Decrypts one DPAPI blob; false when it isn't this user's or is damaged.
bool Unprotect(const uint8_t* data, size_t size, std::vector<uint8_t>& plain) {
  DATA_BLOB in{static_cast<DWORD>(size), const_cast<uint8_t*>(data)}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
  plain.assign(out.pbData, out.pbData + out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return true;
}

bool Protect(const std::vector<uint8_t>& plain, std::vector<uint8_t>& sealed) {
  DATA_BLOB in{static_cast<DWORD>(plain.size()), const_cast<uint8_t*>(plain.data())}, out{};
  if (!CryptProtectData(&in, L"Featherweight recovery", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
    return false;
  sealed.assign(out.pbData, out.pbData + out.cbData);
  LocalFree(out.pbData);
  return true;
}

constexpr char kJournalMagic[4] = {'F', 'W', 'R', 'J'};  // a piece of edits, after the document

// Version 2's file: magic, version, then pieces: a length and a DPAPI blob each.
bool ReadAny(const std::wstring& file, Entry& entry, std::vector<uint8_t>& document, std::vector<uint8_t>* records) {
  std::vector<uint8_t> bytes;
  if (!ReadWhole(file, bytes) || bytes.size() < 8 || std::memcmp(bytes.data(), kFileMagic, 4) != 0) return false;
  uint32_t version = 0;
  std::memcpy(&version, bytes.data() + 4, 4);
  if (version == 0 || version > kVersion) return false;
  entry = Entry{};
  entry.file = file;
  if (records) records->clear();
  std::vector<uint8_t> plain;
  if (version == 1) {
    bool ok = Unprotect(bytes.data() + 8, bytes.size() - 8, plain) && ReadDocument(plain, version, entry, document);
    SecureZeroMemory(plain.data(), plain.size());
    return ok;
  }
  entry.journal = true;
  size_t at = 8;
  for (bool first = true; bytes.size() - at >= 4; first = false) {
    uint32_t size = 0;
    std::memcpy(&size, bytes.data() + at, 4);
    if (size > bytes.size() - at - 4 || !Unprotect(bytes.data() + at + 4, size, plain)) break;  // cut short
    at += 4 + size;
    if (first) {
      if (!ReadDocument(plain, version, entry, document)) return false;
    } else if (plain.size() >= 12 && std::memcmp(plain.data(), kJournalMagic, 4) == 0) {
      std::memcpy(&entry.savedAt, plain.data() + 4, 8);
      if (records) records->insert(records->end(), plain.begin() + 12, plain.end());
    } else {
      break;
    }
    SecureZeroMemory(plain.data(), plain.size());
  }
  SecureZeroMemory(plain.data(), plain.size());
  return !entry.title.empty() || !document.empty();
}

bool WriteFileAtomically(const std::wstring& file, const std::vector<uint8_t>& bytes) {
  const std::wstring tmp = file + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size() &&
            FlushFileBuffers(h);
  CloseHandle(h);
  ok = ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

}  // namespace

bool Read(const std::wstring& file, Entry& entry, std::vector<uint8_t>& document) {
  return ReadAny(file, entry, document, nullptr) && !entry.journal;
}

bool ReadJournal(const std::wstring& file, Entry& entry, std::vector<uint8_t>& document, std::vector<uint8_t>& records) {
  return ReadAny(file, entry, document, &records);
}

bool StartJournal(const std::wstring& file, const std::wstring& originalPath, const std::wstring& title,
                  std::span<const uint8_t> document, std::string_view meta, int64_t now) {
  if (file.empty() || meta.size() > 65536) return false;
  std::vector<uint8_t> payload(kPayloadMagic, kPayloadMagic + 4);
  Put<uint32_t>(payload, kVersion);
  Put<int64_t>(payload, now ? now : Now());
  PutString(payload, originalPath);
  PutString(payload, title);
  Put<uint32_t>(payload, static_cast<uint32_t>(meta.size()));
  payload.insert(payload.end(), meta.begin(), meta.end());
  Put<uint64_t>(payload, document.size());
  payload.insert(payload.end(), document.begin(), document.end());
  std::vector<uint8_t> sealed;
  const bool sealedOk = Protect(payload, sealed);
  SecureZeroMemory(payload.data(), payload.size());
  if (!sealedOk || sealed.size() > UINT32_MAX) return false;
  std::vector<uint8_t> bytes(kFileMagic, kFileMagic + 4);
  Put<uint32_t>(bytes, kVersion);
  Put<uint32_t>(bytes, static_cast<uint32_t>(sealed.size()));
  bytes.insert(bytes.end(), sealed.begin(), sealed.end());
  return WriteFileAtomically(file, bytes);
}

bool AppendJournal(const std::wstring& file, std::span<const uint8_t> records, int64_t now) {
  if (file.empty()) return false;
  if (records.empty()) return true;
  std::vector<uint8_t> payload(kJournalMagic, kJournalMagic + 4);
  Put<int64_t>(payload, now ? now : Now());
  payload.insert(payload.end(), records.begin(), records.end());
  std::vector<uint8_t> sealed;
  const bool sealedOk = Protect(payload, sealed);
  SecureZeroMemory(payload.data(), payload.size());
  if (!sealedOk || sealed.size() > UINT32_MAX) return false;
  std::vector<uint8_t> piece;
  Put<uint32_t>(piece, static_cast<uint32_t>(sealed.size()));
  piece.insert(piece.end(), sealed.begin(), sealed.end());
  // Appended in one write and flushed: a crash leaves the piece whole or cut short (left out).
  HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  const bool ok = WriteFile(h, piece.data(), static_cast<DWORD>(piece.size()), &put, nullptr) && put == piece.size() &&
                  FlushFileBuffers(h);
  CloseHandle(h);
  return ok;
}

std::vector<Entry> FindOrphans(const std::wstring& dir) {
  std::vector<Entry> out;
  if (dir.empty()) return out;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((dir + L"\\session-*.fwr").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) return out;
  std::vector<std::wstring> live, dead;
  do {
    const std::wstring session = SessionOf(fd.cFileName);
    if (session.empty() || std::find(live.begin(), live.end(), session) != live.end()) continue;
    if (std::find(dead.begin(), dead.end(), session) == dead.end()) {
      // Its lock file is held while the session runs. One left behind (a power cut) opens.
      const std::wstring lock = dir + L"\\" + session + L".lock";
      HANDLE h = CreateFileW(lock.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_DELETE_ON_CLOSE, nullptr);
      if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        live.push_back(session);
        continue;
      }
      if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
      dead.push_back(session);
    }
    Entry e;
    std::vector<uint8_t> doc;
    if (ReadAny(dir + L"\\" + fd.cFileName, e, doc, nullptr)) out.push_back(std::move(e));  // journals too
  } while (FindNextFileW(find, &fd));
  FindClose(find);
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.savedAt < b.savedAt; });
  return out;
}

void Remove(const std::wstring& file) {
  if (file.empty()) return;
  SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
  DeleteFileW(file.c_str());
}

}  // namespace fw::pkg::recovery
