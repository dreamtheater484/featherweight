#pragma once

#include <windows.h>

#include <string>

// Group Policy for business deployment (f1-ship): an administrator sets these through the ADMX templates in
// packaging/admx, which write HKLM\SOFTWARE\Policies\Featherweight. Read once, at the first use (the start).
namespace policy {

constexpr wchar_t kKey[] = L"SOFTWARE\\Policies\\Featherweight";
constexpr wchar_t kDisableUpdates[] = L"DisableUpdates";  // REG_DWORD 1: no update checks, no updates
constexpr wchar_t kUpdateChannel[] = L"UpdateChannel";    // REG_SZ "stable" or "beta"
constexpr wchar_t kLanguage[] = L"Language";              // REG_SZ, a language tag such as "de" or "nl-NL"

struct Values {
  bool updatesOff = false;
  std::wstring updateChannel;  // "stable", "beta", or "" when not set (the user's choice applies)
  std::wstring language;       // "" when not set (Windows' display language or the user's choice applies)
};

const Values& Get();
// The policy under any key (tests use a key of their own under HKCU).
Values Read(HKEY root, const wchar_t* subkey);

}  // namespace policy
