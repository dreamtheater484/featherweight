#include "FileAssoc.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "core/Strings.h"

namespace fileassoc {
namespace {

constexpr wchar_t kAppName[] = L"Featherweight";
constexpr wchar_t kClasses[] = L"Software\\Classes\\";
constexpr wchar_t kApplication[] = L"Software\\Classes\\Applications\\Featherweight.exe";
constexpr wchar_t kCapabilities[] = L"Software\\Featherweight\\Capabilities";
constexpr wchar_t kRegisteredApps[] = L"Software\\RegisteredApplications";

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

std::wstring CommandOf(fw::FileKind kind) {
  return GetString(HKEY_CURRENT_USER, kClasses + std::wstring(fw::Info(kind).progId) + L"\\shell\\open\\command",
                   nullptr);
}

// "A light, fast PDF reader", and what else it opens once other viewers ship.
std::wstring Description(const std::vector<fw::FileKind>& kinds) {
  std::vector<std::wstring> others;
  for (fw::FileKind k : kinds)
    if (k != fw::FileKind::Pdf) others.push_back(fw::Info(k).plural);
  std::wstring d = L"A light, fast PDF reader";
  for (size_t i = 0; i < others.size(); ++i)
    d += (i == 0 ? L" that also opens " : i + 1 == others.size() ? L" and " : L", ") + others[i];
  return d;
}

}  // namespace

std::vector<Value> Plan(const std::wstring& exe, const std::vector<fw::FileKind>& kinds) {
  const std::wstring command = OpenCommand(exe);
  const std::wstring appIcon = L"\"" + exe + L"\",0";
  std::vector<Value> v;
  for (fw::FileKind kind : kinds) {
    const fw::FileKindInfo& k = fw::Info(kind);
    const std::wstring progId = kClasses + std::wstring(k.progId);
    v.push_back({progId, L"", k.typeName});
    v.push_back({progId + L"\\DefaultIcon", L"", L"\"" + exe + L"\"," + std::to_wstring(k.iconIndex)});
    v.push_back({progId + L"\\shell\\open\\command", L"", command});
    v.push_back({progId + L"\\Application", L"ApplicationName", kAppName});
    for (const wchar_t* ext : k.extensions)
      v.push_back({kClasses + std::wstring(ext) + L"\\OpenWithProgids", k.progId, L""});
  }

  v.push_back({kApplication, L"FriendlyAppName", kAppName});
  v.push_back({kApplication + std::wstring(L"\\shell\\open\\command"), L"", command});
  for (fw::FileKind kind : kinds)
    for (const wchar_t* ext : fw::Info(kind).extensions)
      v.push_back({kApplication + std::wstring(L"\\SupportedTypes"), ext, L""});

  v.push_back({kCapabilities, L"ApplicationName", kAppName});
  v.push_back({kCapabilities, L"ApplicationDescription", Description(kinds)});
  v.push_back({kCapabilities, L"ApplicationIcon", appIcon});
  for (fw::FileKind kind : kinds)
    for (const wchar_t* ext : fw::Info(kind).extensions)
      v.push_back({kCapabilities + std::wstring(L"\\FileAssociations"), ext, fw::Info(kind).progId});
  v.push_back({kRegisteredApps, kAppName, kCapabilities});
  return v;
}

std::vector<Removal> RemovalPlan() {
  // Every kind, so a registration made by a newer version goes too.
  std::vector<Removal> r;
  for (const fw::FileKindInfo& k : fw::FileKinds()) {
    r.push_back({kClasses + std::wstring(k.progId), L"", true});
    for (auto list : {k.extensions, k.later})
      for (const wchar_t* ext : list) r.push_back({kClasses + std::wstring(ext) + L"\\OpenWithProgids", k.progId});
  }
  r.push_back({kApplication, L"", true});
  r.push_back({kCapabilities, L"", true});
  r.push_back({kRegisteredApps, kAppName});
  return r;
}

bool Register(const std::wstring& exe) {
  bool ok = true;
  for (const Value& v : Plan(exe, fw::ShippedViewers()))
    ok &= SetString(HKEY_CURRENT_USER, v.key, v.name.empty() ? nullptr : v.name.c_str(), v.data);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return ok;
}

void Unregister() {
  for (const Removal& r : RemovalPlan()) {
    if (r.tree) {
      RegDeleteTreeW(HKEY_CURRENT_USER, r.key.c_str());
      continue;
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, r.key.c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
      RegDeleteValueW(key, r.value.c_str());
      RegCloseKey(key);
    }
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool IsRegistered() { return !CommandOf(fw::FileKind::Pdf).empty(); }

std::wstring RegisteredExe() {
  std::wstring cmd = CommandOf(fw::FileKind::Pdf);
  if (cmd.size() < 2 || cmd[0] != L'"') return {};
  size_t end = cmd.find(L'"', 1);
  return end == std::wstring::npos ? std::wstring() : cmd.substr(1, end - 1);
}

bool IsRegisteredAt(const std::wstring& exe) {
  for (fw::FileKind kind : fw::ShippedViewers())
    if (!fw::EqualsNoCase(CommandOf(kind), OpenCommand(exe))) return false;
  return true;
}

bool MissesShippedKinds(const std::wstring& exe) {
  if (!fw::EqualsNoCase(CommandOf(fw::FileKind::Pdf), OpenCommand(exe))) return false;
  for (fw::FileKind kind : fw::ShippedViewers())
    if (CommandOf(kind).empty()) return true;
  return false;
}

bool IsDefault(fw::FileKind kind) {
  IApplicationAssociationRegistration* reg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ApplicationAssociationRegistration, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&reg))))
    return false;
  const fw::FileKindInfo& k = fw::Info(kind);
  LPWSTR progId = nullptr;
  bool result = false;
  if (SUCCEEDED(reg->QueryCurrentDefault(k.extensions[0], AT_FILEEXTENSION, AL_EFFECTIVE, &progId)) && progId) {
    result = fw::EqualsNoCase(progId, k.progId);
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
