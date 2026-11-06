#include "draw/CloudFonts.h"

#include <windows.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <vector>

namespace draw {
namespace {

using Microsoft::WRL::ComPtr;

// Enough for every family Office offers, with room; a folder with more is not Office's.
constexpr size_t kMaxFiles = 512;

struct Cloud {
  ComPtr<IDWriteFontSet> set;
  ComPtr<IDWriteFontCollection1> collection;
};

std::wstring EnglishName(IDWriteLocalizedStrings* names) {
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
  UINT32 length = 0;
  if (FAILED(names->GetStringLength(index, &length))) return {};
  std::wstring name(length + 1, L'\0');
  if (FAILED(names->GetString(index, name.data(), length + 1))) return {};
  name.resize(length);
  return name;
}

struct FontFile {
  std::wstring family;  // the folder it's in: the family Word asked for
  std::wstring path;
};

bool SameName(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// The font files under the folder's family folders.
std::vector<FontFile> FontFiles() {
  std::vector<FontFile> files;
  wchar_t base[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return files;
  const std::wstring root = std::wstring(base) + L"\\Microsoft\\FontCache\\4\\CloudFonts\\";
  WIN32_FIND_DATAW d;
  HANDLE dirs = FindFirstFileExW((root + L"*").c_str(), FindExInfoBasic, &d, FindExSearchLimitToDirectories, nullptr, 0);
  if (dirs == INVALID_HANDLE_VALUE) return files;
  do {
    if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        d.cFileName[0] == L'.')
      continue;
    const std::wstring dir = root + d.cFileName + L"\\";
    WIN32_FIND_DATAW f;
    HANDLE found = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (found == INVALID_HANDLE_VALUE) continue;
    do {
      if (f.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
      std::wstring name = f.cFileName;
      const size_t dot = name.rfind(L'.');
      if (dot == std::wstring::npos) continue;
      std::wstring ext = name.substr(dot);
      for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
      if (ext == L".ttf" || ext == L".otf" || ext == L".ttc") files.push_back({d.cFileName, dir + name});
    } while (files.size() < kMaxFiles && FindNextFileW(found, &f));
    FindClose(found);
  } while (files.size() < kMaxFiles && FindNextFileW(dirs, &d));
  FindClose(dirs);
  return files;
}

const Cloud& Get() {
  static Cloud cloud;
  static std::once_flag once;
  std::call_once(once, [] {
    const std::vector<FontFile> files = FontFiles();
    ComPtr<IDWriteFactory5> factory;
    if (files.empty() || FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                                    reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
      return;
    ComPtr<IDWriteFontCollection> system;
    ComPtr<IDWriteFontCollection1> system1;
    ComPtr<IDWriteFontSet> systemSet;
    ComPtr<IDWriteFontSetBuilder1> keep;
    if (FAILED(factory->GetSystemFontCollection(&system)) || FAILED(system.As(&system1)) ||
        FAILED(system1->GetFontSet(&systemSet)) || FAILED(factory->CreateFontSetBuilder(&keep)))
      return;
    for (const FontFile& f : files) {
      // A file DirectWrite can't read as a font is left out.
      ComPtr<IDWriteFontFile> file;
      ComPtr<IDWriteFontSetBuilder1> one;
      ComPtr<IDWriteFontSet> faces;
      if (FAILED(factory->CreateFontFileReference(f.path.c_str(), nullptr, &file)) || FAILED(factory->CreateFontSetBuilder(&one)) ||
          FAILED(one->AddFontFile(file.Get())) || FAILED(one->CreateFontSet(&faces)))
        continue;
      for (UINT32 i = 0; i < faces->GetFontCount(); ++i) {
        ComPtr<IDWriteLocalizedStrings> names;
        BOOL exists = FALSE;
        if (FAILED(faces->GetPropertyValues(i, DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, &exists, &names)) || !exists) continue;
        // Word finds them by the family it downloaded, the folder's name: "Open Sans Light" (a
        // weight of Open Sans with a family name of its own) is drawn in a substitute, as
        // a missing family (measured on the corpus: Calibri).
        const std::wstring family = EnglishName(names.Get());
        if (!SameName(family, f.family)) continue;
        // A family the PC has stays its own.
        UINT32 index = 0;
        BOOL has = FALSE;
        const DWRITE_FONT_PROPERTY property{DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, family.c_str(), L""};
        ComPtr<IDWriteFontSet> matches;
        if ((SUCCEEDED(system->FindFamilyName(family.c_str(), &index, &has)) && has) ||
            (SUCCEEDED(systemSet->GetMatchingFonts(&property, 1, &matches)) && matches && matches->GetFontCount() > 0))
          continue;
        ComPtr<IDWriteFontFaceReference> ref;
        if (SUCCEEDED(faces->GetFontFaceReference(i, &ref))) keep->AddFontFaceReference(ref.Get());
      }
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(keep->CreateFontSet(&set)) || set->GetFontCount() == 0 ||
        FAILED(factory->CreateFontCollectionFromFontSet(set.Get(), &collection)))
      return;
    cloud.set = set;
    cloud.collection = collection;
  });
  return cloud;
}

}  // namespace

IDWriteFontSet* CloudFontSet() { return Get().set.Get(); }

IDWriteFontCollection1* CloudFontCollection() { return Get().collection.Get(); }

bool CloudFontsHave(std::wstring_view name) {
  IDWriteFontSet* set = CloudFontSet();
  if (!set || name.empty()) return false;
  const std::wstring n(name);
  const DWRITE_FONT_PROPERTY property{DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, n.c_str(), L""};
  ComPtr<IDWriteFontSet> matches;
  return SUCCEEDED(set->GetMatchingFonts(&property, 1, &matches)) && matches && matches->GetFontCount() > 0;
}

}  // namespace draw
