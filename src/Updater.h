#pragma once

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/Json.h"

// The updater's trust rule (f1-ship; plan decision "the updater trusts what it is"). Once the running exe is
// signed, every file it installs must carry a valid Authenticode signature from the same publisher, for the
// expected product; anything else is refused. An unsigned exe (4.0, or 4.1 before SignPath) keeps the SHA-256
// check against the release's SHA256SUMS.txt. The checks never use the network: no revocation checks, and
// certificates are only fetched from the local cache (the update check stays the only network access).
namespace updater {

struct Signature {
  enum class State { Unsigned, Invalid, Trusted };
  State state = State::Unsigned;
  long error = 0;            // WinVerifyTrust's result (0 when trusted)
  std::wstring subject;      // the signing certificate's subject, e.g. "CN=SignPath Foundation, O=..., C=US"
  std::wstring productName;  // from the file's version resource
  std::wstring fileVersion;  // from the file's version resource, e.g. "4.1.0.0"
};

// Reads a file's signature and version resource. Loads wintrust.dll on first use.
Signature ReadSignature(const std::wstring& path);
// The signature of the running exe, read once.
const Signature& RunningSignature();

enum class Verdict { Accepted, NotSigned, BadSignature, OtherPublisher, OtherProduct };
// Whether `file` may replace a file of the running install. With a trusted running exe, `file` must be trusted,
// from the same subject and, when `expectedProduct` isn't empty, have that ProductName. With an unsigned (or
// untrusted) running exe this is always Accepted: the caller checks SHA-256 instead.
Verdict CheckFile(const Signature& running, const Signature& file, const std::wstring& expectedProduct);
const wchar_t* VerdictText(Verdict v);

// Tests only: also treat chains whose root certificate has one of these SHA-1 thumbprints (40 hex digits) as
// trusted, so offline tests can use test certificates that are never added to a certificate store. Empty: off.
void SetTestRootsForTesting(const std::vector<std::wstring>& sha1Thumbprints);

}  // namespace updater

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

  struct Options {
    bool install = false;         // download and swap in the new files (else only report what's available)
    bool beta = false;            // the beta channel: the newest release, GitHub pre-releases included
    std::wstring skippedVersion;  // never installed automatically (Skip this version, or a rollback)
  };

  Updater();
  ~Updater();
  // Runs in the background and posts kDoneMessage to `notify` when finished.
  void Start(HWND notify, const Options& options);
  bool Busy() const;
  Result Get();
  // Waits (briefly) for a file swap in progress, so quitting never leaves a half-updated install.
  void WaitForSwap();

  // Removes leftovers (*.old, *.new) from a previous update. Call at startup. Keeps previous\ (the rollback).
  static void CleanupOldFiles();
  static bool CanSelfUpdate();
  // Inside an MSIX package the Store updates the app, so the updater stays off.
  static bool InPackage();

  // One rollback: an update keeps the files it replaced in previous\, with the version they were.
  static std::wstring PreviousVersion();  // "" when there's nothing to go back to
  // Puts the previous version's files back; it runs from the next start. previous\ is emptied.
  static bool RollBack(std::wstring* error);

  // Test hooks (snapshot mode and unit tests only). The releases URL may be a file: folder holding latest.json
  // (stable) and releases.json (beta), whose assets may be file: paths, for offline tests.
  static void SetReleaseUrlForTesting(const std::wstring& url);
  static void SetInstallDirForTesting(const std::wstring& dir);
  static void SetRunningSignatureForTesting(const updater::Signature* signature);
  static Result RunForTesting(const Options& options);  // synchronous

 private:
  struct Shared {
    std::mutex mutex;
    Result result;
    bool busy = false;
    bool swapping = false;
  };
  static void Run(std::shared_ptr<Shared> shared, HWND notify, Options options);

  // Shared with the worker thread, which may outlive this object while a download finishes.
  std::shared_ptr<Shared> shared_;
};

namespace updater {
// The release to offer from GitHub's answer: /releases/latest (one release) for stable, /releases (a list) for
// beta, where the newest version wins, pre-releases included. Drafts never count. nullptr when there's none.
const fw::JsonValue* PickRelease(const fw::JsonValue& answer, bool beta);
// Whether a pdfium.dll is the build with form scripts (V8 and XFA): it exports FPDF_LoadXFA.
bool IsScriptEngine(const std::wstring& pdfiumPath);
}  // namespace updater