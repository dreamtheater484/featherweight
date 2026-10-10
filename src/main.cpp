#include <ole2.h>
#include <shellapi.h>
#include <windows.h>

#include <string>
#include <vector>

#include "App.h"
#include "FileAssoc.h"
#include "Util.h"
#include "core/Strings.h"

namespace {

// Hands the files to an already running Featherweight. Returns true if that worked.
bool ForwardToRunningInstance(const std::vector<std::wstring>& files) {
  HWND existing = nullptr;
  for (int attempt = 0; attempt < 40 && !existing; ++attempt) {
    existing = FindWindowW(App::kWindowClass, nullptr);
    if (!existing) Sleep(50);
  }
  if (!existing) return false;
  std::wstring joined;
  for (const auto& f : files) joined += util::FullPath(f) + L"\n";
  DWORD pid = 0;
  GetWindowThreadProcessId(existing, &pid);
  AllowSetForegroundWindow(pid);
  COPYDATASTRUCT cds{App::kCopyDataOpen, static_cast<DWORD>(joined.size() * sizeof(wchar_t)), joined.data()};
  DWORD_PTR result = 0;
  if (!SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 5000, &result))
    return false;
  if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
  SetForegroundWindow(existing);
  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int showCommand) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  AppOptions opts;
  opts.showCommand = showCommand;
  bool doRegister = false, doUnregister = false;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
    if (a == L"--register") doRegister = true;
    else if (a == L"--unregister") doUnregister = true;
    else if (a == L"--snapshot") opts.snapshotPath = next();
    else if (a == L"--size") {
      auto parts = fw::Split(next(), L'x');
      if (parts.size() == 2) {
        opts.snapshotW = _wtoi(parts[0].c_str());
        opts.snapshotH = _wtoi(parts[1].c_str());
      }
    } else if (a == L"--scale") opts.snapshotScale = static_cast<float>(_wtof(next().c_str()));
    else if (a == L"--theme") opts.snapshotTheme = next();
    else if (a == L"--invert") opts.snapshotInvert = true;
    else if (a == L"--script") opts.script = next();
    else if (!a.empty() && a[0] != L'-') opts.files.push_back(a);
  }
  LocalFree(argv);

  if (doRegister || doUnregister) {
    if (doRegister) fileassoc::Register(util::ExePath());
    else fileassoc::Unregister();
    return 0;
  }

  const bool snapshot = !opts.snapshotPath.empty();
  HANDLE mutex = nullptr;
  if (!snapshot) {
    mutex = CreateMutexW(nullptr, FALSE, L"Local\\Featherweight-7d1c2f0e-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS && ForwardToRunningInstance(opts.files)) {
      CloseHandle(mutex);
      return 0;
    }
  }

  OleInitialize(nullptr);
  int code = 0;
  {
    App app;
    code = app.Run(opts);
  }
  OleUninitialize();
  if (mutex) CloseHandle(mutex);
  return code;
}
