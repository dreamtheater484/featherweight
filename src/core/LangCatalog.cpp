#include "core/LangCatalog.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <numeric>

namespace fw::lang::catalog {
namespace {

constexpr uint32_t kVersion = 1;      // LANG_<CODE>'s texts
constexpr uint32_t kKeysVersion = 2;  // LANG_KEYS: deltas and packed places

uint32_t Get32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t Step(uint32_t h, uint32_t byte) { return (h ^ (byte & 0xFF)) * 16777619u; }

}  // namespace

uint32_t Hash(uint32_t seed, const wchar_t* context, const wchar_t* en) {
  uint32_t h = 2166136261u ^ seed;
  if (context) {
    for (const wchar_t* p = context; *p; ++p) h = Step(Step(h, *p), static_cast<uint32_t>(*p) >> 8);
    h = Step(Step(h, 4), 0);
  }
  for (const wchar_t* p = en; *p; ++p) h = Step(Step(h, *p), static_cast<uint32_t>(*p) >> 8);
  return h;
}

uint32_t KeysCheck(const Keys& k) {
  uint32_t h = 2166136261u ^ k.seed;
  for (uint32_t i = 0; i < k.count; ++i) {
    for (int b = 0; b < 4; ++b) h = Step(h, k.hashes[i] >> (8 * b));
    h = Step(Step(h, k.places[i]), static_cast<uint32_t>(k.places[i]) >> 8);
  }
  return h;
}

bool ReadKeys(const uint8_t* p, size_t n, Keys& keys) {
  keys = {};
  if (n < 17 || std::memcmp(p, "FWLK", 4) != 0 || Get32(p + 4) != kKeysVersion) return false;
  const uint32_t seed = Get32(p + 8), count = Get32(p + 12);
  if (count == 0 || count > 65536 || count > n - 16) return false;  // a hash takes a byte at least
  std::vector<uint32_t> hashes(count);
  size_t at = 16;
  uint64_t h = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t delta = 0;
    for (int shift = 0;; shift += 7) {
      if (at >= n || shift > 28) return false;
      const uint8_t b = p[at++];
      delta |= static_cast<uint64_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) break;
    }
    if (i > 0 && delta == 0) return false;  // strictly ascending
    h += delta;
    if (h > UINT32_MAX) return false;
    hashes[i] = static_cast<uint32_t>(h);
  }
  if (at >= n) return false;
  const unsigned w = p[at++];
  if (w == 0 || w > 16 || (count - 1) >> w || n - at < (static_cast<size_t>(count) * w + 7) / 8) return false;
  std::vector<uint16_t> places(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v = 0;
    for (unsigned b = 0; b < w; ++b) {
      const size_t bit = static_cast<size_t>(i) * w + b;
      v |= static_cast<uint32_t>((p[at + bit / 8] >> (bit % 8)) & 1) << b;
    }
    if (v >= count) return false;
    places[i] = static_cast<uint16_t>(v);
  }
  keys.seed = seed;
  keys.count = count;
  keys.hashes = std::move(hashes);
  keys.places = std::move(places);
  return true;
}

bool ReadTexts(const uint8_t* p, size_t n, const Keys& keys, std::vector<wchar_t>& text, std::vector<uint32_t>& offsets) {
  if (n < 16 || n > INT32_MAX || std::memcmp(p, "FWLT", 4) != 0 || Get32(p + 4) != kVersion) return false;
  if (Get32(p + 8) != keys.count || Get32(p + 12) != KeysCheck(keys)) return false;
  const char* body = reinterpret_cast<const char*>(p + 16);
  const int len = static_cast<int>(n - 16);
  if (len <= 0 || body[len - 1] != 0) return false;
  const int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, len, nullptr, 0);
  if (w <= 0) return false;
  text.assign(static_cast<size_t>(w), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, len, text.data(), w);
  offsets.assign(keys.count, UINT32_MAX);
  uint32_t k = 0;
  for (size_t i = 0; i < text.size(); ++k) {
    if (k >= keys.count) return false;
    const size_t end = std::wcslen(text.data() + i) + i;
    if (end > i) offsets[k] = static_cast<uint32_t>(i);
    i = end + 1;
  }
  return k == keys.count;
}

std::vector<uint8_t> WriteKeys(uint32_t seed, const std::vector<uint32_t>& hashes) {
  std::vector<uint16_t> order(hashes.size());
  std::iota(order.begin(), order.end(), uint16_t{0});
  std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) { return hashes[a] < hashes[b]; });
  std::vector<uint8_t> out = {'F', 'W', 'L', 'K'};
  Put32(out, kKeysVersion);
  Put32(out, seed);
  Put32(out, static_cast<uint32_t>(hashes.size()));
  uint32_t last = 0;
  for (uint16_t i : order) {
    for (uint32_t d = hashes[i] - last;; d >>= 7) {
      out.push_back(static_cast<uint8_t>((d & 0x7F) | (d >= 0x80 ? 0x80 : 0)));
      if (d < 0x80) break;
    }
    last = hashes[i];
  }
  unsigned w = 1;
  while (w < 16 && (hashes.size() - 1) >> w) ++w;
  out.push_back(static_cast<uint8_t>(w));
  const size_t start = out.size();
  out.resize(start + (hashes.size() * w + 7) / 8, 0);
  for (size_t k = 0; k < order.size(); ++k)
    for (unsigned b = 0; b < w; ++b)
      if ((order[k] >> b) & 1) out[start + (k * w + b) / 8] |= static_cast<uint8_t>(1u << ((k * w + b) % 8));
  return out;
}

std::vector<uint8_t> WriteTexts(const Keys& keys, const std::vector<std::string>& texts) {
  std::vector<uint8_t> out = {'F', 'W', 'L', 'T'};
  Put32(out, kVersion);
  Put32(out, keys.count);
  Put32(out, KeysCheck(keys));
  for (const std::string& t : texts) {
    out.insert(out.end(), t.begin(), t.end());
    out.push_back(0);
  }
  return out;
}

}  // namespace fw::lang::catalog
