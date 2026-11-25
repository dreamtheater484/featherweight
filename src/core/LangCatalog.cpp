#include "core/LangCatalog.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <numeric>

namespace fw::lang::catalog {
namespace {

constexpr uint32_t kVersion = 1;      // LANG_<CODE>'s texts
constexpr uint32_t kKeysVersion = 2;  // LANG_KEYS: Rice-coded differences and packed places

uint32_t Get32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t Step(uint32_t h, uint32_t byte) { return (h ^ (byte & 0xFF)) * 16777619u; }

// The key table's bits, low bits of each byte first.
struct BitReader {
  const uint8_t* p;
  size_t n;       // bytes
  size_t at = 0;  // bits read
  bool Read(unsigned k, uint32_t& v) {
    if (k > 32 || n * 8 - at < k) return false;
    v = 0;
    for (unsigned got = 0; got < k;) {
      const unsigned off = at % 8, take = std::min(8 - off, k - got);
      v |= static_cast<uint32_t>((p[at / 8] >> off) & ((1u << take) - 1)) << got;
      got += take;
      at += take;
    }
    return true;
  }
};

struct BitWriter {
  std::vector<uint8_t>& out;
  size_t at = 0;
  void Write(uint32_t v, unsigned k) {
    for (unsigned b = 0; b < k; ++b, ++at) {
      if (at % 8 == 0) out.push_back(0);
      if ((v >> b) & 1) out.back() |= static_cast<uint8_t>(1u << (at % 8));
    }
  }
};

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
  if (n < 18 || std::memcmp(p, "FWLK", 4) != 0 || Get32(p + 4) != kKeysVersion) return false;
  const uint32_t seed = Get32(p + 8), count = Get32(p + 12);
  const unsigned k = p[16], w = p[17];
  if (count == 0 || count > 65536 || k > 31 || w == 0 || w > 16 || (count - 1) >> w) return false;
  if ((n - 18) * 8 / (k + 1 + w) < count) return false;  // each key takes k + 1 + w bits at least
  BitReader bits{p + 18, n - 18};
  std::vector<uint32_t> hashes(count);
  uint64_t h = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t q = 0;
    for (uint32_t one = 1; one;) {
      if (!bits.Read(1, one)) return false;
      if (one && ++q > (UINT32_MAX >> k)) return false;
    }
    uint32_t low = 0;
    if (!bits.Read(k, low)) return false;
    const uint64_t gap = q << k | low;
    if (i > 0 && gap == 0) return false;  // strictly ascending
    h += gap;
    if (h > UINT32_MAX) return false;
    hashes[i] = static_cast<uint32_t>(h);
  }
  std::vector<uint16_t> places(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v = 0;
    if (!bits.Read(w, v) || v >= count) return false;
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
  std::vector<uint32_t> gaps;
  uint32_t last = 0;
  for (uint16_t i : order) gaps.push_back(hashes[i] - last), last = hashes[i];
  // The k that takes fewest bits; the smallest of equals.
  unsigned k = 0;
  uint64_t best = UINT64_MAX;
  for (unsigned t = 0; t < 32; ++t) {
    uint64_t sum = 0;
    for (uint32_t g : gaps) sum += t + 1 + (g >> t);
    if (sum < best) best = sum, k = t;
  }
  unsigned w = 1;
  while (w < 16 && (hashes.size() - 1) >> w) ++w;
  std::vector<uint8_t> out = {'F', 'W', 'L', 'K'};
  Put32(out, kKeysVersion);
  Put32(out, seed);
  Put32(out, static_cast<uint32_t>(hashes.size()));
  out.push_back(static_cast<uint8_t>(k));
  out.push_back(static_cast<uint8_t>(w));
  BitWriter bits{out};
  for (uint32_t g : gaps) {
    for (uint32_t q = g >> k; q; --q) bits.Write(1, 1);
    bits.Write(0, 1);
    bits.Write(g, k);
  }
  for (uint16_t i : order) bits.Write(i, w);
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
