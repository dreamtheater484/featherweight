#pragma once

// The compiled catalogs' format (F1, f1-lang), shared by the app (src/core/Lang.cpp), the catalog
// compiler (tools/strings) and the fuzzer (tests/fuzz/fuzz_catalog.cpp).
//
// Two kinds of resource (RCDATA):
//   LANG_KEYS: "FWLK", version 1, seed, count, then count hashes of the template's keys, ascending.
//              A key's hash is its index; one table serves every language.
//   LANG_<CODE> (LANG_DE): "FWLZ", the inflated size, then raw deflate of
//              "FWLT", version 1, count, the keys' check (KeysCheck), then count UTF-8 texts in the
//              keys' order, each ended by a 0 (empty: not translated, English shows).
// All numbers are 32-bit little-endian.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fw::lang::catalog {

// FNV-1a over the UTF-16 code units (low byte, then high), from `seed`; with a context, over its units,
// then a 4 (gettext's separator), then the English text's.
uint32_t Hash(uint32_t seed, const wchar_t* context, const wchar_t* en);

// A check of the key table, stored with each language so a language from another build is refused.
uint32_t KeysCheck(const uint32_t* hashes, uint32_t count);

// LANG_KEYS: false unless the header is right and the hashes strictly ascend.
bool ReadKeys(const uint8_t* p, size_t n, uint32_t& seed, const uint32_t*& hashes, uint32_t& count);

// An inflated language: its texts as UTF-16, each ended by a 0, and per key the offset of its text
// (UINT32_MAX when empty). False unless it matches the keys and every text is valid UTF-8.
bool ReadTexts(const uint8_t* p, size_t n, const uint32_t* hashes, uint32_t count, std::vector<wchar_t>& text,
               std::vector<uint32_t>& offsets);

// The writers (tools/strings).
std::vector<uint8_t> WriteKeys(uint32_t seed, const std::vector<uint32_t>& sortedHashes);
std::vector<uint8_t> WriteTexts(const std::vector<uint32_t>& sortedHashes, const std::vector<std::string>& texts);

}  // namespace fw::lang::catalog
