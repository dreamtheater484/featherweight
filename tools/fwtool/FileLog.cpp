// FW_FILE_LOG_DIR (F1, f1-gate's legal.fileopen): every file this fwtool opens or looks for, by any
// module in the process (DirectWrite loading a font file, a folder listed to find files, a DLL),
// logged once each to <dir>\fwtool-<pid>.txt as it happens. The gate checks that none is under
// Office's cloud-font cache.
//
// Everything that opens a file ends in ntdll's NtCreateFile or NtOpenFile, and a probe for one in
// NtQueryAttributesFile or NtQueryFullAttributesFile. Each of those is a short system-call stub;
// the stub is copied to a trampoline and its start replaced by a jump to the logger, which then
// calls the copy. The stub is checked byte for byte first: if Windows ever changes it, nothing is
// patched and the log's first line says "hook: off" (the gate counts that as a failure).
#include <windows.h>
#include <winternl.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace fwtool {
namespace {

// x64 Windows 10 and 11: mov r10,rcx; mov eax,N; test byte ptr [7FFE0308h],1; jne +3; syscall; ret;
// int 2Eh; ret. The system-call number (bytes 4-7) varies, the rest doesn't.
constexpr size_t kStub = 24;
bool IsSyscallStub(const uint8_t* p) {
  static const uint8_t head[] = {0x4C, 0x8B, 0xD1, 0xB8};
  static const uint8_t tail[] = {0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01, 0x75, 0x03,
                                 0x0F, 0x05, 0xC3, 0xCD, 0x2E, 0xC3};
  return std::memcmp(p, head, sizeof head) == 0 && std::memcmp(p + 8, tail, sizeof tail) == 0;
}

using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                        ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtOpenFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
using NtQueryAttributesFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);

NtCreateFileFn g_create = nullptr;
NtOpenFileFn g_open = nullptr;
NtQueryAttributesFn g_query = nullptr;
NtQueryAttributesFn g_queryFull = nullptr;

HANDLE g_log = INVALID_HANDLE_VALUE;
std::mutex g_mutex;
std::unordered_set<std::wstring>* g_seen = nullptr;  // never freed: the hooks run until the process ends
thread_local int t_depth = 0;

void Write(const std::wstring& line) {
  int n = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>(n) + 1, '\n');
  WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), n, nullptr, nullptr);
  DWORD written = 0;
  WriteFile(g_log, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void Record(POBJECT_ATTRIBUTES attributes) {
  if (!attributes || !attributes->ObjectName || !attributes->ObjectName->Buffer || t_depth) return;
  ++t_depth;  // nothing below opens a file, but a hook must never call itself
  std::wstring name(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
  if (attributes->RootDirectory) {
    // Relative to a folder already open (a file found by listing it): the folder's own path first.
    wchar_t dir[MAX_PATH * 2];
    DWORD n = GetFinalPathNameByHandleW(attributes->RootDirectory, dir, MAX_PATH * 2, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n && n < MAX_PATH * 2) name = std::wstring(dir, n) + L"\\" + name;
  }
  for (const wchar_t* prefix : {L"\\??\\", L"\\\\?\\"})
    if (name.starts_with(prefix)) name.erase(0, wcslen(prefix));
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_seen->insert(name).second) Write(name);
  }
  --t_depth;
}

NTSTATUS NTAPI HookCreate(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES o, PIO_STATUS_BLOCK io, PLARGE_INTEGER size,
                          ULONG attrs, ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG eaLength) {
  Record(o);
  return g_create(h, a, o, io, size, attrs, share, disposition, options, ea, eaLength);
}
NTSTATUS NTAPI HookOpen(PHANDLE h, ACCESS_MASK a, POBJECT_ATTRIBUTES o, PIO_STATUS_BLOCK io, ULONG share, ULONG options) {
  Record(o);
  return g_open(h, a, o, io, share, options);
}
NTSTATUS NTAPI HookQuery(POBJECT_ATTRIBUTES o, PVOID info) {
  Record(o);
  return g_query(o, info);
}
NTSTATUS NTAPI HookQueryFull(POBJECT_ATTRIBUTES o, PVOID info) {
  Record(o);
  return g_queryFull(o, info);
}

}  // namespace

void InstallFileLog() {
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"FW_FILE_LOG_DIR", dir, MAX_PATH);
  if (!n || n >= MAX_PATH) return;
  std::wstring path = std::wstring(dir, n) + L"\\fwtool-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
  g_log = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_log == INVALID_HANDLE_VALUE) return;
  g_seen = new std::unordered_set<std::wstring>();
  // The command line, so a log says which run it was.
  std::wstring header;
#if defined(_M_X64)
  auto* trampolines = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  bool ok = trampolines != nullptr;
  // Every stub checked first (all or none is patched), then copied to the trampolines, which are made
  // executable, then each stub's start replaced by mov rax, hook; jmp rax.
  const char* names[4] = {"NtCreateFile", "NtOpenFile", "NtQueryAttributesFile", "NtQueryFullAttributesFile"};
  void* hooks[4] = {reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void*>(&HookOpen),
                    reinterpret_cast<void*>(&HookQuery), reinterpret_cast<void*>(&HookQueryFull)};
  auto* ntdll = GetModuleHandleW(L"ntdll.dll");
  for (int i = 0; ok && i < 4; ++i) {
    auto* target = reinterpret_cast<const uint8_t*>(GetProcAddress(ntdll, names[i]));
    ok = target && IsSyscallStub(target);
  }
  DWORD old = 0;
  if (ok) {
    for (int i = 0; i < 4; ++i) std::memcpy(trampolines + i * 32, GetProcAddress(ntdll, names[i]), kStub);
    ok = VirtualProtect(trampolines, 4096, PAGE_EXECUTE_READ, &old) != FALSE;
  }
  if (ok) {
    g_create = reinterpret_cast<NtCreateFileFn>(trampolines);
    g_open = reinterpret_cast<NtOpenFileFn>(trampolines + 32);
    g_query = reinterpret_cast<NtQueryAttributesFn>(trampolines + 64);
    g_queryFull = reinterpret_cast<NtQueryAttributesFn>(trampolines + 96);
    for (int i = 0; ok && i < 4; ++i) {
      auto* target = reinterpret_cast<uint8_t*>(GetProcAddress(ntdll, names[i]));
      uint8_t jump[12] = {0x48, 0xB8};
      const auto address = reinterpret_cast<uint64_t>(hooks[i]);
      std::memcpy(jump + 2, &address, sizeof address);
      jump[10] = 0xFF, jump[11] = 0xE0;
      DWORD was = 0;
      ok = VirtualProtect(target, sizeof jump, PAGE_EXECUTE_READWRITE, &was) != FALSE;
      if (ok) {
        std::memcpy(target, jump, sizeof jump);
        VirtualProtect(target, sizeof jump, was, &was);
        FlushInstructionCache(GetCurrentProcess(), target, sizeof jump);
      }
    }
  }
  header = ok ? L"hook: on" : L"hook: off (ntdll's system-call stubs aren't the expected ones)";
#else
  header = L"hook: off (x64 only)";
#endif
  header += L" | ";
  header += GetCommandLineW();
  std::lock_guard<std::mutex> lock(g_mutex);
  Write(header);
}

}  // namespace fwtool
