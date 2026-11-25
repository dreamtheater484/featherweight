#include "UserTemplates.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>

#include "core/FileKinds.h"
#include "core/Lang.h"
#include "core/Strings.h"

namespace usertemplates {
namespace {

bool IsFolder(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// HKCU\Software\Microsoft\Office\16.0\<app>\Options, PersonalTemplates (environment strings
// expanded), or empty.
std::wstring OfficeOption(const wchar_t* app) {
  const std::wstring key = std::wstring(L"Software\\Microsoft\\Office\\16.0\\") + app + L"\\Options";
  wchar_t value[MAX_PATH] = {};
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"PersonalTemplates", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr,
                   value, &size) != ERROR_SUCCESS)
    return {};
  return value;
}

}  // namespace

std::vector<std::wstring> Folders() {
  std::vector<std::wstring> out;
  auto add = [&](std::wstring f) {
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
    if (f.empty()) return;
    for (const std::wstring& o : out)
      if (fw::EqualsNoCase(o, f)) return;
    out.push_back(std::move(f));
  };
  for (const wchar_t* app : {T(L"Word"), T(L"Excel"), L"PowerPoint"})
    if (std::wstring f = OfficeOption(app); !f.empty() && IsFolder(f)) add(f);
  PWSTR docs = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
    add(std::wstring(docs) + L"\\Custom Office Templates");
    CoTaskMemFree(docs);
  }
  return out;
}

std::vector<std::wstring> List(size_t max) { return List(max, Folders()); }

std::vector<std::wstring> List(size_t max, const std::vector<std::wstring>& folders) {
  std::vector<const wchar_t*> exts;
  if (fw::EditorShipped(fw::FileKind::Document))
    for (const wchar_t* e : {L".dotx", L".dotm", L".ott"}) exts.push_back(e);
  if (fw::EditorShipped(fw::FileKind::Workbook))
    for (const wchar_t* e : {L".xltx", L".xltm", L".ots"}) exts.push_back(e);
  if (fw::EditorShipped(fw::FileKind::Presentation))
    for (const wchar_t* e : {L".potx", L".potm", L".otp"}) exts.push_back(e);
  std::vector<std::wstring> out;
  if (exts.empty()) return out;
  for (const std::wstring& folder : folders) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) continue;
    int seen = 0;
    do {
      if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) || fd.cFileName[0] == L'~') continue;
      const std::wstring name = fd.cFileName;
      for (const wchar_t* e : exts)
        if (fw::EndsWithNoCase(name, e)) out.push_back(folder + L"\\" + name);
    } while (++seen < 2000 && FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(fw::FileNameOf(a).c_str(), -1, fw::FileNameOf(b).c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  if (out.size() > max) out.resize(max);
  return out;
}

}  // namespace usertemplates
