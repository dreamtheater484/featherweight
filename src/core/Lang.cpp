#include "core/Lang.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/LangCatalog.h"
#include "miniz.h"

namespace fw::lang {
namespace {

// The loaded language: the shared keys (sorted hashes, in the exe's resources) and this language's
// texts in the same order. Null in English.
struct Table {
  uint32_t seed = 0;
  const uint32_t* hashes = nullptr;
  uint32_t count = 0;
  std::vector<wchar_t> text;      // every translation, each ended by a 0
  std::vector<uint32_t> offsets;  // per key: where its text starts, or kNone (untranslated)
  std::vector<std::string> utf8;  // per key, made on first use by T(const char*)
  SRWLOCK utf8Lock = SRWLOCK_INIT;
};
constexpr uint32_t kNone = UINT32_MAX;

Table* g_table = nullptr;
const wchar_t* g_current = L"en";
#ifdef FW_TEST_SCREENS
bool g_pseudo = false;
#endif

int Find(const Table& t, uint32_t h) {
  const uint32_t* end = t.hashes + t.count;
  const uint32_t* it = std::lower_bound(t.hashes, end, h);
  if (it == end || *it != h) return -1;
  const uint32_t i = static_cast<uint32_t>(it - t.hashes);
  return t.offsets[i] == kNone ? -1 : static_cast<int>(i);
}

const void* Resource(const wchar_t* name, DWORD& size) {
  HRSRC r = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10));  // RT_RCDATA
  if (!r) return nullptr;
  HGLOBAL g = LoadResource(nullptr, r);
  size = SizeofResource(nullptr, r);
  return g ? LockResource(g) : nullptr;
}

uint32_t Read32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

std::unique_ptr<Table> Load(const std::wstring& code) {
  DWORD keySize = 0, langSize = 0;
  const auto* keys = static_cast<const uint8_t*>(Resource(L"LANG_KEYS", keySize));
  std::wstring name = L"LANG_" + code;
  CharUpperBuffW(name.data(), static_cast<DWORD>(name.size()));
  const auto* packed = static_cast<const uint8_t*>(Resource(name.c_str(), langSize));
  if (!keys || !packed) return nullptr;
  auto t = std::make_unique<Table>();
  if (!catalog::ReadKeys(keys, keySize, t->seed, t->hashes, t->count)) return nullptr;
  // The texts: "FWLZ", the inflated size, then raw deflate.
  if (langSize < 8 || std::memcmp(packed, "FWLZ", 4) != 0) return nullptr;
  const uint32_t rawSize = Read32(packed + 4);
  if (rawSize > (16u << 20)) return nullptr;
  std::vector<uint8_t> raw(rawSize);
  const size_t got = tinfl_decompress_mem_to_mem(raw.data(), raw.size(), packed + 8, langSize - 8, 0);
  if (got != rawSize) return nullptr;
  if (!catalog::ReadTexts(raw.data(), raw.size(), t->hashes, t->count, t->text, t->offsets)) return nullptr;
  t->utf8.resize(t->count);
  return t;
}

#ifdef FW_TEST_SCREENS
// The pseudo-language: each English text accented, in brackets, with 40 % more length, keeping its
// placeholders (%d, %.1f, {0}), access keys (&), line breaks and a menu's "\tCtrl+O" as they are.
std::wstring Pseudo(std::wstring_view en) {
  static const wchar_t kFrom[] = L"aceinosuyzACEINOSUYZ";
  static const wchar_t kTo[] = L"åçéïñöšüýžÅÇÉÏÑÖŠÜÝŽ";
  const size_t tab = en.find(L'\t');
  const std::wstring_view body = en.substr(0, tab);
  std::wstring out = L"[";
  for (size_t i = 0; i < body.size(); ++i) {
    const wchar_t c = body[i];
    if (c == L'%') {  // a printf placeholder, copied whole
      size_t j = i + 1;
      while (j < body.size() && !wcschr(L"diouxXeEfgGcsSpzn%", body[j])) ++j;
      while (j < body.size() && body[j] == L'z') ++j;
      out.append(body.substr(i, j + 1 - i));
      i = j;
      continue;
    }
    if (c == L'{' && i + 2 < body.size() && iswdigit(body[i + 1]) && body[i + 2] == L'}') {
      out.append(body.substr(i, 3));
      i += 2;
      continue;
    }
    const wchar_t* at = wcschr(kFrom, c);
    out += (c && at) ? kTo[at - kFrom] : c;
  }
  const size_t letters = static_cast<size_t>(std::count_if(body.begin(), body.end(), [](wchar_t c) { return iswalpha(c); }));
  out.append(std::max<size_t>(1, (letters * 2 + 4) / 5), L'·');
  out += L']';
  if (tab != std::wstring_view::npos) out.append(en.substr(tab));
  return out;
}

SRWLOCK g_pseudoLock = SRWLOCK_INIT;
std::unordered_map<std::wstring, std::wstring>* g_pseudoWide = nullptr;
std::unordered_map<std::string, std::string>* g_pseudoUtf8 = nullptr;

const wchar_t* PseudoWide(const wchar_t* en) {
  AcquireSRWLockExclusive(&g_pseudoLock);
  if (!g_pseudoWide) g_pseudoWide = new std::unordered_map<std::wstring, std::wstring>;
  auto [it, added] = g_pseudoWide->try_emplace(en);
  if (added) it->second = Pseudo(en);
  const wchar_t* r = it->second.c_str();
  ReleaseSRWLockExclusive(&g_pseudoLock);
  return r;
}
#endif

std::wstring Widen(const char* s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
  return w;
}

std::string Narrow(const wchar_t* s) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string u(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, u.data(), n, nullptr, nullptr);
  return u;
}

}  // namespace

const std::vector<Language>& Languages() {
  static const std::vector<Language> kAll = {
      {L"en", L"English"},
      {L"nl", L"Nederlands"},
      {L"de", L"Deutsch"},
      {L"fr", L"Français"},
      {L"es", L"Español"},
#ifdef FW_TEST_SCREENS
      {L"qps", L"Pseudo"},
#endif
  };
  return kAll;
}

std::wstring WindowsLanguage() {
  ULONG count = 0, size = 0;
  if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size == 0) return L"en";
  std::wstring list(size, L'\0');
  if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, list.data(), &size)) return L"en";
  for (const wchar_t* p = list.c_str(); *p; p += wcslen(p) + 1) {
    std::wstring primary(p, wcscspn(p, L"-_"));
    CharLowerBuffW(primary.data(), static_cast<DWORD>(primary.size()));
    for (const Language& l : Languages())
      if (primary == l.code && primary != L"qps") return primary;
  }
  return L"en";
}

void Init(const std::wstring& chosen) {
  std::wstring code;
  for (const Language& l : Languages())
    if (chosen == l.code) code = l.code;
  if (code.empty()) code = WindowsLanguage();
  for (const Language& l : Languages())
    if (code == l.code) g_current = l.code;
#ifdef FW_TEST_SCREENS
  if (code == L"qps") {
    g_pseudo = true;
    return;
  }
#endif
  if (code == L"en") return;
  std::unique_ptr<Table> t = Load(code);
  if (!t) {
    g_current = L"en";
    return;
  }
  g_table = t.release();  // kept for the process's life: T()'s results point into it
}

const wchar_t* Current() { return g_current; }

}  // namespace fw::lang

using fw::lang::g_table;

const wchar_t* T(const wchar_t* en) {
#ifdef FW_TEST_SCREENS
  if (fw::lang::g_pseudo && en) return fw::lang::PseudoWide(en);
#endif
  if (!g_table || !en) return en;
  const int i = fw::lang::Find(*g_table, fw::lang::catalog::Hash(g_table->seed, nullptr, en));
  return i < 0 ? en : g_table->text.data() + g_table->offsets[i];
}

const wchar_t* T(const wchar_t* en, const wchar_t* context) {
#ifdef FW_TEST_SCREENS
  if (fw::lang::g_pseudo && en) return fw::lang::PseudoWide(en);
#endif
  if (!g_table || !en) return en;
  const int i = fw::lang::Find(*g_table, fw::lang::catalog::Hash(g_table->seed, context, en));
  return i < 0 ? en : g_table->text.data() + g_table->offsets[i];
}

const char16_t* T(const char16_t* en) {
  static_assert(sizeof(char16_t) == sizeof(wchar_t));
  return reinterpret_cast<const char16_t*>(T(reinterpret_cast<const wchar_t*>(en)));
}

const char* T(const char* en) {
#ifdef FW_TEST_SCREENS
  if (fw::lang::g_pseudo && en) {
    AcquireSRWLockExclusive(&fw::lang::g_pseudoLock);
    if (!fw::lang::g_pseudoUtf8) fw::lang::g_pseudoUtf8 = new std::unordered_map<std::string, std::string>;
    auto [it, added] = fw::lang::g_pseudoUtf8->try_emplace(en);
    if (added) it->second = fw::lang::Narrow(fw::lang::Pseudo(fw::lang::Widen(en)).c_str());
    const char* r = it->second.c_str();
    ReleaseSRWLockExclusive(&fw::lang::g_pseudoLock);
    return r;
  }
#endif
  if (!g_table || !en) return en;
  const std::wstring w = fw::lang::Widen(en);
  const int i = fw::lang::Find(*g_table, fw::lang::catalog::Hash(g_table->seed, nullptr, w.c_str()));
  if (i < 0) return en;
  fw::lang::Table& t = *g_table;
  AcquireSRWLockExclusive(&t.utf8Lock);
  std::string& s = t.utf8[static_cast<size_t>(i)];
  if (s.empty()) s = fw::lang::Narrow(t.text.data() + t.offsets[static_cast<size_t>(i)]);
  const char* r = s.c_str();
  ReleaseSRWLockExclusive(&t.utf8Lock);
  return r;
}
