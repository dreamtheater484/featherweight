// fwtool bench-app: how fast Featherweight starts and opens files, and how much memory and CPU it
// uses (plan §11). It runs a copy of the app and watches it from outside, so it measures any
// build the same way, 0.9.4 included, and never touches the user's settings or recent files.
#include <windows.h>

#include <psapi.h>
#include <timeapi.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

#include "Common.h"
#include "core/FileKinds.h"
#include "core/Strings.h"

#pragma comment(lib, "version.lib")
#pragma comment(lib, "winmm.lib")

namespace fwtool {
namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// ---------- Safety: never hand files to a running Featherweight, never change its registration ----------

std::wstring RegString(HKEY root, const std::wstring& key) {
  wchar_t buf[1024];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return buf;
}

// Why a launch isn't safe right now, or empty.
std::string Unsafe() {
  if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\Featherweight-7d1c2f0e-instance")) {
    CloseHandle(m);
    return "Featherweight is running. Close it first: a copy started without --sandbox (0.9.4 and other older "
           "builds) would hand its file to it.";
  }
  // A Featherweight registered from a folder that no longer exists makes an older build point "Open with" at
  // whatever copy starts next: the copy measured here.
  std::wstring cmd = RegString(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") +
                                                      fw::Info(fw::FileKind::Pdf).progId + L"\\shell\\open\\command");
  if (cmd.size() > 2 && cmd[0] == L'"') {
    std::wstring exe = cmd.substr(1, cmd.find(L'"', 1) - 1);
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
      return "Featherweight is registered from " + fw::WideToUtf8(exe) +
             ", which no longer exists, so an older build would register the copy measured here. Register the "
             "installed copy again first (Featherweight.exe --register).";
  }
  return {};
}

// ---------- What is measured ----------

std::wstring ProductVersion(const std::wstring& exe) {
  DWORD handle = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
  std::vector<uint8_t> data(size);
  wchar_t* value = nullptr;
  UINT len = 0;
  if (!size || !GetFileVersionInfoW(exe.c_str(), 0, size, data.data()) ||
      !VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\ProductVersion", reinterpret_cast<void**>(&value),
                      &len) ||
      !value)
    return {};
  return value;
}

// Memory the process has to itself: its private working set, as Task Manager shows it, and its
// private bytes (committed).
struct Memory {
  double privateWorkingSetMB = 0, privateBytesMB = 0, workingSetMB = 0;
};

Memory MemoryOf(HANDLE process) {
  Memory m;
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    m.privateBytesMB = pmc.PrivateUsage / 1048576.0;
    m.workingSetMB = pmc.WorkingSetSize / 1048576.0;
  }
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  size_t entries = pmc.WorkingSetSize / si.dwPageSize + 4096;
  for (int attempt = 0; attempt < 4; ++attempt, entries *= 2) {
    std::vector<ULONG_PTR> buf(entries + 1);
    if (QueryWorkingSet(process, buf.data(), static_cast<DWORD>(buf.size() * sizeof(ULONG_PTR)))) {
      auto* ws = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buf.data());
      size_t privatePages = 0;
      for (ULONG_PTR i = 0; i < ws->NumberOfEntries; ++i) privatePages += ws->WorkingSetInfo[i].Shared ? 0 : 1;
      m.privateWorkingSetMB = privatePages * si.dwPageSize / 1048576.0;
      break;
    }
  }
  return m;
}

double CpuMs(HANDLE process) {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  auto ticks = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  return (ticks(kernel) + ticks(user)) / 10000.0;
}

// Thread context switches of a process: each is a thread waking up (a timer, a message, a wait
// that ended). SYSTEM_PROCESS_INFORMATION is followed by one of these per thread.
struct SystemThread {
  LARGE_INTEGER kernelTime, userTime, createTime;
  ULONG waitTime;
  PVOID startAddress;
  HANDLE clientProcess, clientThread;
  LONG priority, basePriority;
  ULONG contextSwitches, threadState, waitReason;
};

uint64_t ContextSwitches(DWORD pid) {
  using Query = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
  static auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  if (!query) return 0;
  std::vector<uint8_t> buf(1 << 20);
  ULONG needed = 0;
  while (query(SystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &needed) < 0) {
    if (buf.size() > (64u << 20)) return 0;
    buf.resize(std::max<size_t>(buf.size() * 2, needed + 65536));
  }
  for (size_t offset = 0;;) {
    auto* p = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(buf.data() + offset);
    if (static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(p->UniqueProcessId)) == pid) {
      auto* threads = reinterpret_cast<SystemThread*>(p + 1);
      uint64_t total = 0;
      for (ULONG t = 0; t < p->NumberOfThreads; ++t) total += threads[t].contextSwitches;
      return total;
    }
    if (!p->NextEntryOffset) return 0;
    offset += p->NextEntryOffset;
  }
}

// ---------- Watching the window ----------

// The main window shown, from the WinEvent hook.
HWND g_shown = nullptr;
DWORD g_pid = 0;
Clock::time_point g_shownAt;

bool IsMainWindow(HWND hwnd) {
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  return GetAncestor(hwnd, GA_ROOT) == hwnd &&
         (std::wstring_view(cls) == L"FeatherweightWindow" || std::wstring_view(cls) == L"FeatherweightSandboxWindow");
}

void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
  if (g_shown || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == g_pid && IsMainWindow(hwnd)) {
    g_shownAt = Clock::now();
    g_shown = hwnd;
  }
}

void Pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
}

// The window's client area as seen on screen, in blocks: each the mean brightness of its pixels.
class Screen {
 public:
  static constexpr int kCols = 80, kRows = 60;
  using Frame = std::vector<uint8_t>;

  bool Capture(HWND hwnd, Frame& out) {
    RECT rc;
    if (!GetClientRect(hwnd, &rc) || rc.right < kCols || rc.bottom < kRows) return false;
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    int w = rc.right, h = rc.bottom;
    if (w != w_ || h != h_) Resize(w, h);
    // From the screen, not PrintWindow: that makes the app paint again for every capture.
    HDC screen = GetDC(nullptr);
    BitBlt(mem_, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);
    ReleaseDC(nullptr, screen);
    GdiFlush();
    out.assign(kCols * kRows, 0);
    std::vector<uint32_t> sum(kCols * kRows, 0), count(kCols * kRows, 0);
    for (int y = 0; y < h; y += 2) {
      const uint8_t* row = bits_ + static_cast<size_t>(y) * w * 4;
      int by = y * kRows / h;
      for (int x = 0; x < w; x += 2) {
        int b = by * kCols + x * kCols / w;
        sum[b] += (row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) / 3;
        ++count[b];
      }
    }
    for (int i = 0; i < kCols * kRows; ++i) out[i] = static_cast<uint8_t>(count[i] ? sum[i] / count[i] : 0);
    return true;
  }

  // The share of blocks that differ visibly.
  static double Difference(const Frame& a, const Frame& b) {
    int n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) n += std::abs(a[i] - b[i]) > 6;
    return static_cast<double>(n) / (kCols * kRows);
  }

  ~Screen() {
    if (mem_) DeleteDC(mem_);
    if (dib_) DeleteObject(dib_);
  }

 private:
  void Resize(int w, int h) {
    if (mem_) DeleteDC(mem_);
    if (dib_) DeleteObject(dib_);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    dib_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    mem_ = CreateCompatibleDC(nullptr);
    SelectObject(mem_, dib_);
    bits_ = static_cast<uint8_t*>(bits);
    w_ = w;
    h_ = h;
  }

  HDC mem_ = nullptr;
  HBITMAP dib_ = nullptr;
  uint8_t* bits_ = nullptr;
  int w_ = 0, h_ = 0;
};

// ---------- One run ----------

// Milliseconds from the launch.
struct Run {
  std::string error;
  double windowMs = -1;  // the main window shown
  // The app's last work before it goes quiet: the window drawn and, with a file, its first page.
  // Read from the CPU cycles of its UI thread, which asking doesn't disturb. Other threads are
  // Windows' (the thread pool, the graphics driver) and stay busy, or retire, on their own clock.
  double readyMs = -1;
  // The window first looking the way it settles on screen. This includes Windows' own animation of
  // a window opening, which runs alongside the app's work.
  double onScreenMs = -1;
  Memory memory;  // once settled
  double idleCpuMs = -1, idleWakeups = -1;  // in all, over kIdleSeconds once settled
};

// Long enough to catch a timer that fires once a second, which one-off wakeups (Windows retiring an
// idle thread) don't add up to.
constexpr double kIdleSeconds = 3;
// Less work than this between two looks, about half a millisecond, isn't busy: Windows pokes a new
// window now and then (its icon, the end of its animation), which takes the UI thread a few
// microseconds.
constexpr ULONG64 kBusyCycles = 2000000;

void WriteSettings(const std::filesystem::path& ini, const std::wstring& version) {
  // For builds without --sandbox: no update check, no notices, the same theme each time.
  std::ofstream out(ini, std::ios::binary | std::ios::trunc);
  out << "[General]\r\nTheme=light\r\nUpdateAutomatically=false\r\nLastUpdateCheck="
      << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
      << "\r\nLastRunVersion=" << fw::WideToUtf8(version) << "\r\nOfferedDefaultApp=true\r\n";
}

Run RunOnce(const std::filesystem::path& exe, const std::wstring& version, const std::filesystem::path& file,
            bool idle) {
  Run run;
  if (std::string why = Unsafe(); !why.empty()) {
    run.error = why;
    return run;
  }
  WriteSettings(exe.parent_path() / L"Featherweight.ini", version);
  std::wstring cmd = L"\"" + exe.wstring() + L"\" --sandbox --theme light";
  if (!file.empty()) cmd += L" \"" + file.wstring() + L"\"";
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESHOWWINDOW;
  // Windows uses this for the app's first ShowWindow(SW_SHOWNORMAL): shown, but without taking
  // the keyboard from whatever the user is typing in.
  si.wShowWindow = SW_SHOWNOACTIVATE;
  PROCESS_INFORMATION pi{};
  g_shown = nullptr;
  Clock::time_point t0 = Clock::now();
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                      exe.parent_path().c_str(), &si, &pi)) {
    run.error = "couldn't start " + Display(exe);
    return run;
  }
  g_pid = pi.dwProcessId;
  HWINEVENTHOOK hook =
      SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, pi.dwProcessId, 0, WINEVENT_OUTOFCONTEXT);
  ResumeThread(pi.hThread);
  ULONG64 cycles = 0, lastCycles = 0;
  double busyUntil = 0;
  auto watchCpu = [&] {
    if (QueryThreadCycleTime(pi.hThread, &cycles) && cycles - lastCycles > kBusyCycles) busyUntil = MsSince(t0);
    lastCycles = cycles;
  };
  while (!g_shown && MsSince(t0) < 15000) {
    if (MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 1, QS_ALLINPUT) == WAIT_OBJECT_0) break;
    Pump();
    watchCpu();
  }
  UnhookWinEvent(hook);
  HWND hwnd = g_shown;
  if (!hwnd) {
    run.error = WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0 ? "it quit before showing a window"
                                                                      : "no window within 15 s";
  } else {
    run.windowMs = std::chrono::duration<double, std::milli>(g_shownAt - t0).count();
    // On top, so the screen shows it. Then watch until it has looked the same for a second and
    // the app has done nothing for half a second.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Screen screen;
    std::vector<std::pair<double, Screen::Frame>> frames;
    Screen::Frame frame;
    double stableSince = MsSince(t0);
    while (MsSince(t0) < 20000 && (MsSince(t0) - stableSince < 1000 || MsSince(t0) - busyUntil < 500)) {
      Pump();
      watchCpu();
      double at = MsSince(t0);
      if (screen.Capture(hwnd, frame) && (frames.empty() || Screen::Difference(frames.back().second, frame) > 0)) {
        if (frames.empty() || Screen::Difference(frames.back().second, frame) > 0.002) stableSince = at;
        frames.emplace_back(at, frame);
      }
      Sleep(1);
    }
    run.readyMs = busyUntil;
    // When it first looked (almost) the way it settled.
    for (const auto& [at, f] : frames) {
      if (Screen::Difference(f, frames.back().second) <= 0.005) {
        run.onScreenMs = at;
        break;
      }
    }
    run.memory = MemoryOf(pi.hProcess);
    // Moving the mouse over the window or typing wakes the app: then measure again, or not at all.
    for (int attempt = 0; idle && attempt < 3; ++attempt) {
      LASTINPUTINFO before{sizeof(before)}, after{sizeof(after)};
      GetLastInputInfo(&before);
      double cpu = CpuMs(pi.hProcess);
      uint64_t switches = ContextSwitches(pi.dwProcessId);
      Sleep(static_cast<DWORD>(kIdleSeconds * 1000));
      GetLastInputInfo(&after);
      if (after.dwTime != before.dwTime) continue;
      run.idleCpuMs = CpuMs(pi.hProcess) - cpu;
      run.idleWakeups = static_cast<double>(ContextSwitches(pi.dwProcessId) - switches);
      break;
    }
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
  if (WaitForSingleObject(pi.hProcess, 10000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return run;
}

double Median(std::vector<double> v) {
  v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return x < 0; }), v.end());
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}

std::string Num(double v, int decimals = 1) {
  if (v < 0) return "null";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

std::string JsonString(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

}  // namespace

int RunBenchApp(const Options& options) {
  std::filesystem::path source = std::filesystem::absolute(options.inputs.front());
  if (!std::filesystem::exists(source)) {
    std::fprintf(stderr, "%s doesn't exist.\n", Display(source).c_str());
    return 1;
  }
  if (std::string why = Unsafe(); !why.empty()) {
    std::fprintf(stderr, "%s\n", why.c_str());
    return 1;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  timeBeginPeriod(1);

  // A private copy: settings and recent files land next to it (portable mode), never in the user's.
  std::error_code ec;
  std::filesystem::path sandbox =
      std::filesystem::temp_directory_path(ec) / (L"fwtool-bench-app-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(sandbox, ec);
  std::filesystem::path exe = sandbox / L"Featherweight.exe";
  std::filesystem::copy_file(source, exe, std::filesystem::copy_options::overwrite_existing, ec);
  std::filesystem::path pdfium = source.parent_path() / L"pdfium.dll";
  if (std::filesystem::exists(pdfium))
    std::filesystem::copy_file(pdfium, sandbox / L"pdfium.dll", std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::fprintf(stderr, "Couldn't copy Featherweight to %s.\n", Display(sandbox).c_str());
    return 1;
  }
  std::wstring version = ProductVersion(source);

  std::vector<std::filesystem::path> files{std::filesystem::path()};
  for (size_t i = 1; i < options.inputs.size(); ++i) files.push_back(std::filesystem::absolute(options.inputs[i]));
  const int runs = std::max(1, options.runs);
  std::printf("%s %s: the median of %d runs after a warm-up, each a fresh start, in ms from the launch.\n",
              Display(source).c_str(), fw::WideToUtf8(version).c_str(), runs);
  std::printf("\"ready\" is the app's last work before it goes quiet; \"on screen\" includes Windows' animation of the\n"
              "window opening. Memory is the private working set once settled; idle is CPU time and thread wakeups\n"
              "in all, over %.0f s after that.\n\n",
              kIdleSeconds);
  std::printf("%-24s %8s %8s %10s %9s %13s %9s %8s\n", "", "window", "ready", "on screen", "memory", "private bytes",
              "idle CPU", "wakeups");

  std::string json = "{\n  \"format\": \"featherweight-app-bench\",\n  \"version\": 1,\n  \"exe\": " +
                     JsonString(fw::WideToUtf8(source.generic_wstring())) + ",\n  \"productVersion\": " +
                     JsonString(fw::WideToUtf8(version)) + ",\n  \"exeBytes\": " +
                     std::to_string(std::filesystem::file_size(source, ec)) + ",\n  \"runs\": " + std::to_string(runs) +
                     ",\n  \"idleSeconds\": " + Num(kIdleSeconds, 0) + ",\n  \"scenarios\": [";
  int failed = 0;
  for (size_t s = 0; s < files.size(); ++s) {
    const auto& file = files[s];
    std::string name = file.empty() ? "launch" : fw::WideToUtf8(file.filename().wstring());
    RunOnce(exe, version, file, false);  // warm-up: the files in the cache
    std::vector<Run> results;
    for (int r = 0; r < runs; ++r) results.push_back(RunOnce(exe, version, file, r == 0));
    std::string error;
    std::vector<double> window, ready, onScreen, memory, privateBytes;
    for (const Run& r : results) {
      if (!r.error.empty()) error = r.error;
      window.push_back(r.windowMs);
      ready.push_back(r.readyMs);
      onScreen.push_back(r.onScreenMs);
      memory.push_back(r.error.empty() ? r.memory.privateWorkingSetMB : -1);
      privateBytes.push_back(r.error.empty() ? r.memory.privateBytesMB : -1);
    }
    if (!error.empty()) {
      std::printf("%-24s failed: %s\n", name.c_str(), error.c_str());
      json += std::string(s ? ",\n" : "\n") + "    {\"name\": " + JsonString(name) + ", \"error\": " +
              JsonString(error) + "}";
      ++failed;
      if (error.starts_with("Featherweight is r")) break;
      continue;
    }
    const Run& first = results.front();
    char idle[32] = "input, not measured";
    if (first.idleCpuMs >= 0)
      std::snprintf(idle, sizeof(idle), "%6s ms %8s", Num(first.idleCpuMs).c_str(), Num(first.idleWakeups, 0).c_str());
    std::printf("%-24s %8s %8s %10s %6s MB %10s MB %18s\n", name.c_str(), Num(Median(window)).c_str(),
                Num(Median(ready)).c_str(), Num(Median(onScreen)).c_str(), Num(Median(memory)).c_str(),
                Num(Median(privateBytes)).c_str(), idle);
    json += std::string(s ? ",\n" : "\n") + "    {\"name\": " + JsonString(name) +
            ", \"windowMs\": " + Num(Median(window)) + ", \"readyMs\": " + Num(Median(ready)) +
            ", \"onScreenMs\": " + Num(Median(onScreen)) + ", \"memoryMB\": " + Num(Median(memory)) +
            ", \"privateBytesMB\": " + Num(Median(privateBytes)) + ", \"idleCpuMs\": " + Num(first.idleCpuMs) +
            ", \"idleWakeups\": " + Num(first.idleWakeups, 0) + "}";
  }
  json += "\n  ]\n}\n";
  timeEndPeriod(1);
  std::filesystem::remove_all(sandbox, ec);
  if (!options.jsonOut.empty()) {
    std::ofstream out(options.jsonOut, std::ios::binary);
    out << json;
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace fwtool
