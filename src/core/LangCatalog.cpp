#include "core/LangCatalog.h"

#include <windows.h>

#include <cstring>

namespace fw::lang::catalog {
namespace {

constexpr uint32_t kVersion = 1;

uint32_t Get32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t Step(uint32_t h, wchar_t c) {
  h = (h ^ (static_cast<uint32_t>(c) & 0xFF)) * 16777619u;
  return (h ^ (static_cast<uint32_t>(c) >> 8)) * 16777619u;
}

}  // namespace

uint32_t Hash(uint32_t seed, const wchar_t* context, const wchar_t* en) {
  uint32_t h = 2166136261u ^ seed;
  if (context) {
    for (const wchar_t* p = context; *p; ++p) h = Step(h, *p);
    h = Step(h, 4);
  }
  for (const wchar_t* p = en; *p; ++p) h = Step(h, *p);
  return h;
}

uint32_t KeysCheck(const uint32_t* hashes, uint32_t count) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < count; ++i)
    for (int b = 0; b < 4; ++b) h = (h ^ ((hashes[i] >> (8 * b)) & 0xFF)) * 16777619u;
  return h;
}

bool ReadKeys(const uint8_t* p, size_t n, uint32_t& seed, const uint32_t*& hashes, uint32_t& count) {
  if (n < 16 || std::memcmp(p, "FWLK", 4) != 0 || Get32(p + 4) != kVersion) return false;
  seed = Get32(p + 8);
  count = Get32(p + 12);
  if (count == 0 || count > (n - 16) / 4) return false;
  hashes = reinterpret_cast<const uint32_t*>(p + 16);
  for (uint32_t i = 1; i < count; ++i)
    if (hashes[i - 1] >= hashes[i]) return false;
  return true;
}

bool ReadTexts(const uint8_t* p, size_t n, const uint32_t* hashes, uint32_t count, std::vector<wchar_t>& text,
               std::vector<uint32_t>& offsets) {
  if (n < 16 || std::memcmp(p, "FWLT", 4) != 0 || Get32(p + 4) != kVersion) return false;
  if (Get32(p + 8) != count || Get32(p + 12) != KeysCheck(hashes, count)) return false;
  const char* body = reinterpret_cast<const char*>(p + 16);
  const int len = static_cast<int>(n - 16);
  if (len <= 0 || body[len - 1] != 0) return false;
  const int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, len, nullptr, 0);
  if (w <= 0) return false;
  text.assign(static_cast<size_t>(w), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, len, text.data(), w);
  offsets.assign(count, UINT32_MAX);
  uint32_t k = 0;
  for (size_t i = 0; i < text.size(); ++k) {
    if (k >= count) return false;
    const size_t end = std::wcslen(text.data() + i) + i;
    if (end > i) offsets[k] = static_cast<uint32_t>(i);
    i = end + 1;
  }
  return k == count;
}

std::vector<uint8_t> WriteKeys(uint32_t seed, const std::vector<uint32_t>& sortedHashes) {
  std::vector<uint8_t> out = {'F', 'W', 'L', 'K'};
  Put32(out, kVersion);
  Put32(out, seed);
  Put32(out, static_cast<uint32_t>(sortedHashes.size()));
  for (uint32_t h : sortedHashes) Put32(out, h);
  return out;
}

std::vector<uint8_t> WriteTexts(const std::vector<uint32_t>& sortedHashes, const std::vector<std::string>& texts) {
  std::vector<uint8_t> out = {'F', 'W', 'L', 'T'};
  Put32(out, kVersion);
  Put32(out, static_cast<uint32_t>(sortedHashes.size()));
  Put32(out, KeysCheck(sortedHashes.data(), static_cast<uint32_t>(sortedHashes.size())));
  for (const std::string& t : texts) {
    out.insert(out.end(), t.begin(), t.end());
    out.push_back(0);
  }
  return out;
}

}  // namespace fw::lang::catalog
