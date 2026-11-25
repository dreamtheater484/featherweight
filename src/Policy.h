#pragma once

#include <windows.h>

#include <string>

// Group Policy for business deployment (f1-ship): an administrator sets these through the ADMX templates in
// packaging/admx, which write HKLM\SOFTWARE\Policies\Featherweight. Read once, at the first use (the start).
namespace policy {

// The key and value names (packaging\admx\Featherweight.admx writes the same; a unit test checks).
extern const wchar_t kKey[];             // SOFTWARE\Policies\Featherweight
extern const wchar_t kDisableUpdates[];  // REG_DWORD 1: no update checks, no updates
extern const wchar_t kUpdateChannel[];   // REG_SZ "stable" or "beta"
extern const wchar_t kLanguage[];        // REG_SZ: en, nl, de, fr or es

struct Values {
  bool updatesOff = false;
  std::wstring updateChannel;  // "stable", "beta", or "" when not set (the user's choice applies)
  std::wstring language;       // "" when not set (Windows' display language or the user's choice applies)
};

const Values& Get();
// The policy under any key (tests use a key of their own under HKCU).
Values Read(HKEY root, const wchar_t* subkey);

}  // namespace policy
