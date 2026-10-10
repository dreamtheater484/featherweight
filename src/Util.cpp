#include "Util.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>

#include <ctime>

#include "core/Strings.h"

namespace util {

std::wstring ExePath() {
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n < buf.size()) {
      buf.resize(n);
      return buf;
    }
    buf.resize(buf.size() * 2);
  }
}

std::wstring ExeDir() { return fw::DirOf(ExePath()); }

bool FileExists(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryWritable(const std::wstring& dir) {
  std::wstring probe = dir + L"\\.fw-write-test-" + std::to_wstring(GetCurrentProcessId());
  HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
}

std::wstring AppDataDir() {
  PWSTR path = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &path))) {
    dir = std::wstring(path) + L"\\Featherweight";
    CoTaskMemFree(path);
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  return dir;
}

std::wstring FullPath(const std::wstring& path) {
  DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (n == 0) return path;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
  out.resize(n);
  return out;
}

bool ReadFileBytes(const std::wstring& path, std::string& out, size_t maxBytes) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(h, &size) && static_cast<unsigned long long>(size.QuadPart) <= maxBytes;
  if (ok) {
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (ok && done < out.size()) {
      DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 16u << 20));
      DWORD read = 0;
      ok = ReadFile(h, out.data() + done, chunk, &read, nullptr) && read > 0;
      done += read;
    }
  }
  CloseHandle(h);
  return ok;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
  std::wstring tmp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
  ok = ok && FlushFileBuffers(h);
  CloseHandle(h);
  if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

void SetClipboardText(const std::wstring& text) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* p = GlobalLock(h)) {
      memcpy(p, text.c_str(), bytes);
      GlobalUnlock(h);
      if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    } else {
      GlobalFree(h);
    }
  }
  CloseClipboard();
}

std::wstring GetClipboardText() {
  std::wstring out;
  if (!OpenClipboard(nullptr)) return out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
      out = p;
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

int64_t UnixNow() { return static_cast<int64_t>(std::time(nullptr)); }

namespace {

std::string HexDigest(const unsigned char* d, size_t n) {
  static const char* hex = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s += hex[d[i] >> 4];
    s += hex[d[i] & 15];
  }
  return s;
}

class Sha256Hasher {
 public:
  Sha256Hasher() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
    BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0);
  }
  ~Sha256Hasher() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  bool Ok() const { return hash_ != nullptr; }
  void Update(const void* data, size_t n) {
    BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(n), 0);
  }
  std::string Finish() {
    unsigned char digest[32];
    if (BCryptFinishHash(hash_, digest, sizeof(digest), 0) != 0) return {};
    return HexDigest(digest, sizeof(digest));
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

}  // namespace

std::string Sha256(const void* data, size_t size) {
  Sha256Hasher h;
  if (!h.Ok()) return {};
  h.Update(data, size);
  return h.Finish();
}

std::string Sha256File(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return {};
  Sha256Hasher h;
  std::string result;
  if (h.Ok()) {
    std::vector<char> buf(1 << 20);
    DWORD read = 0;
    bool ok = true;
    for (;;) {
      ok = ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) != FALSE;
      if (!ok || read == 0) break;
      h.Update(buf.data(), read);
    }
    if (ok) result = h.Finish();
  }
  CloseHandle(f);
  return result;
}

bool IsSafeExternalLink(const std::wstring& url) {
  std::wstring u = fw::ToLower(fw::Trim(url));
  return u.rfind(L"https://", 0) == 0 || u.rfind(L"http://", 0) == 0 || u.rfind(L"mailto:", 0) == 0;
}

bool OpenExternalLink(const std::wstring& url) {
  if (!IsSafeExternalLink(url)) return false;
  auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", fw::Trim(url).c_str(), nullptr, nullptr,
                                                   SW_SHOWNORMAL));
  return r > 32;
}

float DpiScale(HWND hwnd) {
  UINT dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
  return dpi ? dpi / 96.0f : 1.0f;
}

std::wstring LastErrorMessage(DWORD error) {
  wchar_t* buf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 error, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
  std::wstring s = buf ? fw::Trim(buf) : L"";
  if (buf) LocalFree(buf);
  return s;
}

}  // namespace util
