#pragma once

#include <windows.h>

#include <string>

// Registers Featherweight as a PDF app for the current user only (no admin rights needed).
namespace fileassoc {

bool Register(const std::wstring& exePath);
void Unregister();
// True if registered, and the registration points at `exePath`.
bool IsRegisteredAt(const std::wstring& exePath);
bool IsRegistered();
// True if Windows currently opens .pdf files with Featherweight.
bool IsDefault();
// Opens Windows Settings on the page where the user confirms Featherweight as the default.
void OpenDefaultAppsSettings();

}  // namespace fileassoc
