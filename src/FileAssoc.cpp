#include "FileAssoc.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "core/Strings.h"

namespace fileassoc {
namespace {

constexpr wchar_t kProgId[] = L"Featherweight.PDF";
constexpr wchar_t kAppName[] = L"Featherweight";

bool SetString(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& value) {
  HKEY key;
  if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return false;
  LSTATUS s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                             static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return s == ERROR_SUCCESS;
}

std::wstring GetString(HKEY root, const std::wstring& subkey, const wchar_t* name) {
  wchar_t buf[2048];
  DWORD size = sizeof(buf);
  if (RegGetValueW(root, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return buf;
}

std::wstring OpenCommand(const std::wstring& exe) { return L"\"" + exe + L"\" \"%1\""; }

}  // namespace

bool Register(const std::wstring& exe) {
  const std::wstring classes = L"Software\\Classes\\";
  const std::wstring icon = L"\"" + exe + L"\",0";
  bool ok = true;
  ok &= SetString(HKEY_CURRENT_USER, classes + kProgId, nullptr, L"PDF Document");
  ok &= SetString(HKEY_CURRENT_USER, classes + kProgId + L"\\DefaultIcon", nullptr, icon);
  ok &= SetString(HKEY_CURRENT_USER, classes + kProgId + L"\\shell\\open\\command", nullptr, OpenCommand(exe));
  ok &= SetString(HKEY_CURRENT_USER, classes + kProgId + L"\\Application", L"ApplicationName", kAppName);
  ok &= SetString(HKEY_CURRENT_USER, classes + L".pdf\\OpenWithProgids", kProgId, L"");

  const std::wstring app = classes + L"Applications\\Featherweight.exe";
  ok &= SetString(HKEY_CURRENT_USER, app, L"FriendlyAppName", kAppName);
  ok &= SetString(HKEY_CURRENT_USER, app + L"\\shell\\open\\command", nullptr, OpenCommand(exe));
  ok &= SetString(HKEY_CURRENT_USER, app + L"\\SupportedTypes", L".pdf", L"");

  const std::wstring caps = L"Software\\Featherweight\\Capabilities";
  ok &= SetString(HKEY_CURRENT_USER, caps, L"ApplicationName", kAppName);
  ok &= SetString(HKEY_CURRENT_USER, caps, L"ApplicationDescription", L"A light, fast PDF reader");
  ok &= SetString(HKEY_CURRENT_USER, caps, L"ApplicationIcon", icon);
  ok &= SetString(HKEY_CURRENT_USER, caps + L"\\FileAssociations", L".pdf", kProgId);
  ok &= SetString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppName, caps);

  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return ok;
}

void Unregister() {
  RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + kProgId).c_str());
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.pdf\\OpenWithProgids", 0, KEY_SET_VALUE, &key) ==
      ERROR_SUCCESS) {
    RegDeleteValueW(key, kProgId);
    RegCloseKey(key);
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\Featherweight.exe");
  RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Featherweight\\Capabilities");
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
    RegDeleteValueW(key, kAppName);
    RegCloseKey(key);
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool IsRegistered() {
  return !GetString(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + kProgId + L"\\shell\\open\\command",
                    nullptr)
              .empty();
}

std::wstring RegisteredExe() {
  std::wstring cmd =
      GetString(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + kProgId + L"\\shell\\open\\command", nullptr);
  if (cmd.size() < 2 || cmd[0] != L'"') return {};
  size_t end = cmd.find(L'"', 1);
  return end == std::wstring::npos ? std::wstring() : cmd.substr(1, end - 1);
}

bool IsRegisteredAt(const std::wstring& exe) {
  std::wstring cmd =
      GetString(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\") + kProgId + L"\\shell\\open\\command", nullptr);
  return fw::EqualsNoCase(cmd, OpenCommand(exe));
}

bool IsDefault() {
  IApplicationAssociationRegistration* reg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ApplicationAssociationRegistration, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&reg))))
    return false;
  LPWSTR progId = nullptr;
  bool result = false;
  if (SUCCEEDED(reg->QueryCurrentDefault(L".pdf", AT_FILEEXTENSION, AL_EFFECTIVE, &progId)) && progId) {
    result = fw::EqualsNoCase(progId, kProgId);
    CoTaskMemFree(progId);
  }
  reg->Release();
  return result;
}

void OpenDefaultAppsSettings() {
  // Windows 11 can open Featherweight's own page in Default apps; Windows 10 opens the general page.
  OSVERSIONINFOEXW v{sizeof(v)};
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (rtl) rtl(&v);
  const wchar_t* uri = v.dwBuildNumber >= 22000 ? L"ms-settings:defaultapps?registeredAppUser=Featherweight"
                                                : L"ms-settings:defaultapps";
  ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace fileassoc
