// Fuzz target: the compiled language catalogs (src/core/LangCatalog.cpp, F1 f1-lang), as the app
// reads them at start: a key table (LANG_KEYS) and one language's texts once inflated (LANG_<CODE>).
// The input is the two, one after the other: the key table's size (32-bit, little-endian), its
// bytes, then the texts' bytes. Seeds: tests/fuzz/seeds/catalog (make_catalog_seeds.py).
//
// It aborts when:
// - a key table that reads has hashes out of order, a place out of range, or doesn't read back the
//   same once written again from the template's order;
// - texts that read have a count other than the table's, an offset outside the text or a text
//   without its ending 0, or don't read back the same once written again as UTF-8.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/LangCatalog.h"

using namespace fw::lang::catalog;

namespace {

void Check(bool ok) {
  if (!ok) std::abort();
}

std::string Utf8(const wchar_t* s) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 4) return 0;
  uint32_t keysSize;
  std::memcpy(&keysSize, data, 4);
  // Each part in its own allocation, aligned as a resource is, so the reader's 32-bit loads are too.
  const size_t k = std::min<size_t>(keysSize, size - 4);
  const std::vector<uint8_t> keyBytes(data + 4, data + 4 + k), textBytes(data + 4 + k, data + size);

  Keys keys;
  if (!ReadKeys(keyBytes.data(), keyBytes.size(), keys)) return 0;
  Check(keys.count > 0 && keys.count <= 65536);
  std::vector<uint32_t> order(keys.count);
  std::vector<bool> placed(keys.count);
  bool permutation = true;
  for (uint32_t i = 0; i < keys.count; ++i) {
    Check(i == 0 || keys.hashes[i - 1] < keys.hashes[i]);
    Check(keys.places[i] < keys.count);
    if (placed[keys.places[i]]) permutation = false;
    placed[keys.places[i]] = true;
    order[keys.places[i]] = keys.hashes[i];
  }
  if (permutation) {  // what the compiler writes: the table again from the template's order
    const std::vector<uint8_t> again = WriteKeys(keys.seed, order);
    Keys read;
    Check(ReadKeys(again.data(), again.size(), read));
    Check(read.count == keys.count && read.seed == keys.seed && KeysCheck(read) == KeysCheck(keys));
    Check(read.hashes == keys.hashes && read.places == keys.places);
  }

  std::vector<wchar_t> text;
  std::vector<uint32_t> offsets;
  if (!ReadTexts(textBytes.data(), textBytes.size(), keys, text, offsets)) return 0;
  Check(offsets.size() == keys.count);
  std::vector<std::string> utf8(keys.count);
  for (uint32_t i = 0; i < keys.count; ++i) {
    if (offsets[i] == UINT32_MAX) continue;
    Check(offsets[i] < text.size());
    Check(std::find(text.begin() + offsets[i], text.end(), L'\0') != text.end());
    Check(text[offsets[i]] != L'\0');  // an empty text is "not translated", never an offset
    utf8[i] = Utf8(text.data() + offsets[i]);
  }
  const std::vector<uint8_t> again = WriteTexts(keys, utf8);
  std::vector<wchar_t> text2;
  std::vector<uint32_t> offsets2;
  Check(ReadTexts(again.data(), again.size(), keys, text2, offsets2));
  for (uint32_t i = 0; i < keys.count; ++i) {
    Check((offsets[i] == UINT32_MAX) == (offsets2[i] == UINT32_MAX));
    if (offsets[i] != UINT32_MAX) Check(std::wcscmp(text.data() + offsets[i], text2.data() + offsets2[i]) == 0);
  }
  return 0;
}
