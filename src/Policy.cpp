#include "Policy.h"

#include <cwchar>

namespace policy {

const wchar_t kKey[] = L"SOFTWARE\\Policies\\Featherweight";
const wchar_t kDisableUpdates[] = L"DisableUpdates";
const wchar_t kUpdateChannel[] = L"UpdateChannel";
const wchar_t kLanguage[] = L"Language";

Values Read(HKEY root, const wchar_t* subkey) {
  Values v;
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return v;
  DWORD off = 0, size = sizeof(off), type = 0;
  if (RegQueryValueExW(key, kDisableUpdates, nullptr, &type, reinterpret_cast<BYTE*>(&off), &size) == ERROR_SUCCESS &&
      type == REG_DWORD)
    v.updatesOff = off != 0;
  auto text = [&](const wchar_t* name) {
    wchar_t buf[64] = {};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ))
      return std::wstring();
    return std::wstring(buf, wcsnlen(buf, bytes / sizeof(wchar_t)));
  };
  std::wstring channel = text(kUpdateChannel);
  if (_wcsicmp(channel.c_str(), L"stable") == 0) v.updateChannel = L"stable";
  else if (_wcsicmp(channel.c_str(), L"beta") == 0) v.updateChannel = L"beta";
  v.language = text(kLanguage);
  RegCloseKey(key);
  return v;
}

const Values& Get() {
  static const Values values = Read(HKEY_LOCAL_MACHINE, kKey);
  return values;
}

}  // namespace policy
