#include "CrashReport.h"

#include <dbghelp.h>  // types only: dbghelp.dll is loaded when a crash happens
#include <shellapi.h>
#include <signal.h>
#include <stdlib.h>

#include <cstring>
#include <cwchar>
#include <exception>
#include <vector>

#include "Util.h"

namespace crash {
namespace {

constexpr wchar_t kRepo[] = L"https://github.com/dreamtheater484/featherweight";
constexpr wchar_t kSite[] = L"https://dreamtheater484.github.io/featherweight/";
constexpr wchar_t kNoteName[] = L"pending.txt";
constexpr size_t kKeep = 5;
// The code raised for crashes that come without an exception (std::terminate, a pure call, an invalid
// CRT parameter, abort), so their dump has a context too. Bit 29 marks it as an application's code.
constexpr DWORD kFatalCode = 0xE0F70001;

// Copied at Install, so the crash path needs no heap.
wchar_t g_folder[MAX_PATH] = {};
wchar_t g_version[32] = {};
volatile LONG g_crashing = 0;

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

void WindowsVersionInto(wchar_t* out, size_t n) {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW v{sizeof(v)};
  auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (!fn || fn(&v) != 0) v = {};
  swprintf_s(out, n, L"%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
}

bool WriteAll(HANDLE f, const void* data, DWORD size) {
  DWORD written = 0;
  return WriteFile(f, data, size, &written, nullptr) && written == size;
}

// The crash path: stack buffers only, no heap, no CRT locks.
bool WriteDumpRaw(const wchar_t* folder, EXCEPTION_POINTERS* ep, DWORD thread, const wchar_t* version) {
  if (!folder[0] || !ep || !ep->ExceptionRecord) return false;
  wchar_t parent[MAX_PATH];
  wcscpy_s(parent, folder);
  if (wchar_t* slash = wcsrchr(parent, L'\\')) {
    *slash = 0;
    CreateDirectoryW(parent, nullptr);  // %LOCALAPPDATA%\Featherweight, if nothing made it yet
  }
  CreateDirectoryW(folder, nullptr);

  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[96], path[MAX_PATH + 96];
  swprintf_s(name, L"Featherweight-%s-%04u%02u%02u-%02u%02u%02u.dmp", version, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
  swprintf_s(path, L"%s\\%s", folder, name);

  // Only from System32, so a dbghelp.dll beside the exe or a document is never loaded.
  HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
  bool dumped = false;
  if (write) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION mei{thread, ep, FALSE};
      // Stacks, threads and modules: small (a few hundred KB), and no heap, so no more of the open
      // documents than their stacks hold.
      const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
      dumped = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr) != FALSE;
      CloseHandle(f);
      if (!dumped) DeleteFileW(path);
    }
  }

  // The note: what the next start offers to report. UTF-16, one key=value a line.
  const void* address = ep->ExceptionRecord->ExceptionAddress;
  HMODULE module = nullptr;
  wchar_t modulePath[MAX_PATH] = L"?";
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(address), &module))
    GetModuleFileNameW(module, modulePath, MAX_PATH);
  const wchar_t* moduleName = wcsrchr(modulePath, L'\\') ? wcsrchr(modulePath, L'\\') + 1 : modulePath;
  const unsigned long long offset =
      module ? reinterpret_cast<ULONG_PTR>(address) - reinterpret_cast<ULONG_PTR>(module) : reinterpret_cast<ULONG_PTR>(address);
  wchar_t windows[32];
  WindowsVersionInto(windows, 32);
  wchar_t note[1024];
  const int len = swprintf_s(note, L"\xFEFFversion=%s\r\ncode=0x%08lX\r\nmodule=%s\r\noffset=0x%llX\r\nwindows=%s\r\ndump=%s\r\n",
                             version, ep->ExceptionRecord->ExceptionCode, moduleName, offset, windows, dumped ? name : L"");
  wchar_t notePath[MAX_PATH + 16];
  swprintf_s(notePath, L"%s\\%s", folder, kNoteName);
  HANDLE f = CreateFileW(notePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    if (len > 0) WriteAll(f, note, static_cast<DWORD>(len * sizeof(wchar_t)));
    CloseHandle(f);
  }
  return dumped;
}

struct Job {
  EXCEPTION_POINTERS* ep;
  DWORD thread;
};

DWORD WINAPI Writer(void* p) {
  auto* job = static_cast<Job*>(p);
  WriteDumpRaw(g_folder, job->ep, job->thread, g_version);
  return 0;
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
  if (InterlockedExchange(&g_crashing, 1)) return EXCEPTION_EXECUTE_HANDLER;  // a crash while writing one
  // On a fresh thread: the faulting one may have run out of stack.
  Job job{ep, GetCurrentThreadId()};
  if (HANDLE t = CreateThread(nullptr, 256 * 1024, Writer, &job, 0, nullptr)) {
    WaitForSingleObject(t, 60 * 1000);
    CloseHandle(t);
  } else {
    WriteDumpRaw(g_folder, ep, job.thread, g_version);
  }
  return EXCEPTION_EXECUTE_HANDLER;  // ends the process; the next start offers the report
}

[[noreturn]] void RaiseFatal(ULONG_PTR kind) {
  __try {
    RaiseException(kFatalCode, EXCEPTION_NONCONTINUABLE, 1, &kind);
  } __except (Filter(GetExceptionInformation())) {
  }
  TerminateProcess(GetCurrentProcess(), kFatalCode);
  __assume(0);
}

void OnTerminate() { RaiseFatal(1); }
void OnPureCall() { RaiseFatal(2); }
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { RaiseFatal(3); }
void OnAbort(int) { RaiseFatal(4); }

std::wstring PercentEncode(const std::wstring& text) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(n > 0 ? n : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), n, nullptr, nullptr);
  std::wstring out;
  for (unsigned char c : utf8) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += static_cast<wchar_t>(c);
    } else {
      wchar_t hex[4];
      swprintf_s(hex, L"%%%02X", c);
      out += hex;
    }
  }
  return out;
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

void Install(const std::wstring& folder, const std::wstring& version) {
  wcsncpy_s(g_folder, folder.c_str(), _TRUNCATE);
  wcsncpy_s(g_version, version.c_str(), _TRUNCATE);
  SetUnhandledExceptionFilter(Filter);
  std::set_terminate(OnTerminate);
  _set_purecall_handler(OnPureCall);
  _set_invalid_parameter_handler(OnInvalidParameter);
  signal(SIGABRT, OnAbort);
}

bool WriteDump(const std::wstring& folder, EXCEPTION_POINTERS* ep, DWORD thread, const std::wstring& version) {
  return WriteDumpRaw(folder.c_str(), ep, thread, version.c_str());
}

void Prune(const std::wstring& folder, size_t keep) {
  struct Dump {
    FILETIME time;
    std::wstring name;
  };
  std::vector<Dump> dumps;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((folder + L"\\*.dmp").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) dumps.push_back({fd.ftLastWriteTime, fd.cFileName});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  // A handful of files: remove the oldest until `keep` are left (no sort, which costs kilobytes).
  while (dumps.size() > keep) {
    auto oldest = dumps.begin();
    for (auto it = dumps.begin(); it != dumps.end(); ++it)
      if (CompareFileTime(&it->time, &oldest->time) < 0) oldest = it;
    DeleteFileW((folder + L"\\" + oldest->name).c_str());
    dumps.erase(oldest);
  }
}

std::optional<Pending> TakePending(const std::wstring& folder) {
  if (folder.empty()) return std::nullopt;
  const std::wstring notePath = folder + L"\\" + kNoteName;
  if (!FileExists(notePath)) return std::nullopt;  // the usual case: one call, nothing else
  std::string bytes;
  const bool read = util::ReadFileBytes(notePath, bytes, 64 * 1024);
  DeleteFileW(notePath.c_str());
  Prune(folder, kKeep);
  if (!read || bytes.size() < 2) return std::nullopt;
  // The note is UTF-16: copied, not cast, so a byte string is never read as a wide one.
  std::wstring text(bytes.size() / sizeof(wchar_t), L'\0');
  memcpy(text.data(), bytes.data(), text.size() * sizeof(wchar_t));
  if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
  Pending p;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(L'\n', start);
    if (end == std::wstring::npos) end = text.size();
    std::wstring line = text.substr(start, end - start);
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    start = end + 1;
    const size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) continue;
    const std::wstring key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == L"version") p.version = value;
    else if (key == L"code") p.code = value;
    else if (key == L"module") p.module = value;
    else if (key == L"offset") p.offset = value;
    else if (key == L"windows") p.windows = value;
    else if (key == L"dump") p.dump = value;
  }
  // A dump name is a plain file name in that folder, nothing else.
  if (p.dump.find_first_of(L"\\/:") != std::wstring::npos) p.dump.clear();
  return p;
}

std::wstring WindowsVersion() {
  wchar_t v[32];
  WindowsVersionInto(v, 32);
  return v;
}

std::wstring IssueUrl(const Pending* crash, const std::wstring& version) {
  std::wstring title, body;
  const std::wstring about = L"**Featherweight** " + version + L", **Windows** " + WindowsVersion() + L"\n";
  if (crash) {
    title = L"Crash: " + crash->code + L" in " + crash->module;
    body = L"Featherweight closed unexpectedly.\n\n**What were you doing when it happened?**\n\n\n"
           L"**Crash:** " + crash->code + L" in " + crash->module + L"+" + crash->offset +
           (crash->version != version ? L" (in " + crash->version + L")" : L"") + L"\n" + about;
    if (!crash->dump.empty())
      body += L"\nThe crash dump helps find the cause: zip `" + crash->dump +
              L"` (right-click it, then Compress to ZIP file) and drag the zip here. It may hold bits of the "
              L"documents that were open, so don't attach it if any of them were private.\n";
  } else {
    body = L"**What happened, and what did you expect?**\n\n\n**Steps to see it again:**\n\n1. \n\n" + about +
           L"\nIf a file shows the problem and it isn't private, attach it (zipped).\n";
  }
  std::wstring url = std::wstring(kRepo) + L"/issues/new?";
  if (!title.empty()) url += L"title=" + PercentEncode(title) + L"&";
  return url + L"body=" + PercentEncode(body);
}

std::wstring HelpUrl() { return std::wstring(kSite) + L"help/"; }
std::wstring KnownIssuesUrl() { return std::wstring(kSite) + L"known-issues/"; }

void ShowInFolder(const std::wstring& folder, const std::wstring& dump) {
  const std::wstring path = folder + L"\\" + dump;
  if (!dump.empty() && FileExists(path)) {
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  } else {
    CreateDirectoryW(folder.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
}

void ShowNotices(const std::wstring& version) {
  const std::wstring path = util::ExeDir() + L"\\THIRD_PARTY_NOTICES.md";
  if (FileExists(path)) {
    const std::wstring args = L"\"" + path + L"\"";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
      return;
  }
  util::OpenExternalLink(std::wstring(kRepo) + L"/blob/v" + version + L"/THIRD_PARTY_NOTICES.md");
}

}  // namespace crash
