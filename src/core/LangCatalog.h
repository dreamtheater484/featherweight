#pragma once

// The compiled catalogs' format (F1, f1-lang), shared by the app (src/core/Lang.cpp), the catalog
// compiler (tools/strings/fwstrings.cpp) and the fuzzer (tests/fuzz/fuzz_catalog.cpp).
//
// Two kinds of resource (RCDATA):
//   LANG_KEYS: "FWLK", version 2, seed, count, a byte k, a byte w, then bits (low bits of each byte
//              first): the keys' hashes, ascending, each as its difference from the one before (the
//              first from 0) in Rice code (the difference >> k as that many 1 bits and a 0, then
//              its low k bits), then each hash's place among the texts in w bits. One table serves
//              every language.
//   LANG_<CODE> (LANG_DE): "FWLZ", the inflated size, then raw deflate of
//              "FWLT", version 1, count, the keys' check (KeysCheck), then count UTF-8 texts in the
//              template's order (sorted by their English, which compresses best), each ended by a 0
//              (empty: not translated, English shows).
// All numbers are little-endian; the ones in the headers 32-bit.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fw::lang::catalog {

// FNV-1a over the UTF-16 code units (low byte, then high), from `seed`; with a context, over its units,
// then a 4 (gettext's separator), then the English text's.
uint32_t Hash(uint32_t seed, const wchar_t* context, const wchar_t* en);

struct Keys {
  uint32_t seed = 0;
  uint32_t count = 0;
  std::vector<uint32_t> hashes;  // ascending
  std::vector<uint16_t> places;  // per hash: the key's place in the template
};

// A check of the key table, stored with each language so a language from another build is refused.
uint32_t KeysCheck(const Keys& k);

// LANG_KEYS: false unless the header is right, the hashes strictly ascend and the places are in range.
bool ReadKeys(const uint8_t* p, size_t n, Keys& keys);

// An inflated language: its texts as UTF-16, each ended by a 0, and per place the offset of its text
// (UINT32_MAX when empty). False unless it matches the keys and every text is valid UTF-8.
bool ReadTexts(const uint8_t* p, size_t n, const Keys& keys, std::vector<wchar_t>& text, std::vector<uint32_t>& offsets);

// The writers (tools/strings). `hashes` in the template's order; WriteKeys sorts them.
std::vector<uint8_t> WriteKeys(uint32_t seed, const std::vector<uint32_t>& hashes);
std::vector<uint8_t> WriteTexts(const Keys& keys, const std::vector<std::string>& texts);

}  // namespace fw::lang::catalog
