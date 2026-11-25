#pragma once

// Local crash reports and the project's help links (F1, f1-trust; docs/f1-plan.md "Support").
//
// A crash writes a minidump into the Crashes folder, with a note the next start reads; at most 5
// dumps are kept. The next start offers a prefilled GitHub issue and the folder, so the user can
// attach the dump. Featherweight itself never sends anything: the issue opens in the user's browser.
// Nothing loads at launch: dbghelp.dll is loaded only when a crash happens.

#include <windows.h>

#include <optional>
#include <string>

namespace crash {

// Catches crashes from now on: unhandled exceptions, std::terminate, pure calls and invalid CRT
// parameters. `folder` and `version` are copied; the folder is created when the first dump is written.
void Install(const std::wstring& folder, const std::wstring& version);

// What the last crash left in the folder's note.
struct Pending {
  std::wstring version;  // "4.1.0"
  std::wstring code;     // "0xC0000005"
  std::wstring module;   // "Featherweight.exe": the module the exception happened in, no path
  std::wstring offset;   // "0x1A2B3C": the address within that module
  std::wstring windows;  // "10.0.26100"
  std::wstring dump;     // the dump's file name, no path
};

// The note the last crash left, if any. The note is removed, and the folder keeps the 5 newest dumps.
std::optional<Pending> TakePending(const std::wstring& folder);

// Writes a dump of this process for the exception, and the note. `thread` is the faulting thread.
// The crash handler calls it; the tests call it with an exception they catch.
bool WriteDump(const std::wstring& folder, EXCEPTION_POINTERS* ep, DWORD thread, const std::wstring& version);

// Removes all but the `keep` newest dumps (by name, which starts with the time).
void Prune(const std::wstring& folder, size_t keep);

// Windows' version, as "10.0.26100".
std::wstring WindowsVersion();

// A new GitHub issue with the title and body filled in: the version, Windows' version and, for a
// crash, the exception and where it happened. Never a path, a user name or a file's content.
std::wstring IssueUrl(const Pending* crash, const std::wstring& version);

// The website's pages, opened in the user's browser from Help.
std::wstring HelpUrl();
std::wstring KnownIssuesUrl();

// Shows the dump in Explorer (selected), or the folder when the dump is gone.
void ShowInFolder(const std::wstring& folder, const std::wstring& dump);

// Opens THIRD_PARTY_NOTICES.md beside the exe in Notepad, or the release's copy on GitHub when it
// isn't there (a development build).
void ShowNotices(const std::wstring& version);

}  // namespace crash
