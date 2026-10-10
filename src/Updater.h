#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>

// Checks GitHub Releases for a newer version and, if allowed, swaps in the new files quietly.
// The new version takes effect the next time Featherweight starts.
class Updater {
 public:
  enum class Status { Idle, Checking, UpToDate, Available, Installed, Failed };
  struct Result {
    Status status = Status::Idle;
    std::wstring version;  // e.g. "1.1.0"
    std::wstring pageUrl;  // release page
    std::wstring error;
  };

  static constexpr UINT kDoneMessage = WM_APP + 20;

  Updater();
  ~Updater();
  // Runs in the background and posts kDoneMessage to `notify` when finished.
  void Start(HWND notify, bool install);
  bool Busy() const;
  Result Get();
  // Waits (briefly) for a file swap in progress, so quitting never leaves a half-updated install.
  void WaitForSwap();

  // Removes leftovers (*.old) from a previous update. Call at startup.
  static void CleanupOldFiles();
  static bool CanSelfUpdate();
  // Test hook (snapshot mode only): check a different releases URL.
  static void SetReleaseUrlForTesting(const std::wstring& url);

 private:
  struct Shared {
    std::mutex mutex;
    Result result;
    bool busy = false;
    bool swapping = false;
  };
  static void Run(std::shared_ptr<Shared> shared, HWND notify, bool install);

  // Shared with the worker thread, which may outlive this object while a download finishes.
  std::shared_ptr<Shared> shared_;
};
