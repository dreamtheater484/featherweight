#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

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

  ~Updater();
  // Runs in the background and posts kDoneMessage to `notify` when finished.
  void Start(HWND notify, bool install);
  bool Busy() const { return busy_; }
  Result Get();

  // Removes leftovers (*.old) from a previous update. Call at startup.
  static void CleanupOldFiles();
  static bool CanSelfUpdate();

 private:
  void Run(HWND notify, bool install);

  std::thread thread_;
  std::atomic<bool> busy_{false};
  std::mutex mutex_;
  Result result_;
};
