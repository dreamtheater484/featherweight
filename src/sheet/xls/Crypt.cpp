// Encrypted .xls files ([MS-XLS] 2.2.10, [MS-OFFCRYPTO] 2.3.6): RC4 (Excel 97) or RC4 CryptoAPI
// (Excel 2002 and later). The FILEPASS record holds the encryption header; every record after it
// has its data encrypted with RC4, in 1,024-byte blocks of the stream, each block with a key of its
// own, the key stream running from each block's start (record headers are left clear but use up the
// key stream). BOF, FILEPASS, INTERFACEHDR and a few others stay clear, and so does BOUNDSHEET's
// stream position. Excel encrypts with the password "VelvetSweatshop" when only the workbook's
// structure is protected, so those files open without asking.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "sheet/xls/Internal.h"

#pragma comment(lib, "bcrypt.lib")

namespace fw::sheet::xls::detail {
namespace {

using Buf = std::vector<uint8_t>;

bool Hash(LPCWSTR alg, std::initializer_list<std::span<const uint8_t>> parts, Buf& out) {
  BCRYPT_ALG_HANDLE a = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  DWORD size = 0, got = 0;
  bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&a, alg, nullptr, 0)) &&
            BCRYPT_SUCCESS(BCryptGetProperty(a, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof size, &got, 0)) &&
            BCRYPT_SUCCESS(BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0));
  for (const auto& p : parts)
    if (ok && !p.empty()) ok = BCRYPT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(p.data()), ULONG(p.size()), 0));
  if (ok) {
    out.assign(size, 0);
    ok = BCRYPT_SUCCESS(BCryptFinishHash(h, out.data(), ULONG(out.size()), 0));
  }
  if (h) BCryptDestroyHash(h);
  if (a) BCryptCloseAlgorithmProvider(a, 0);
  return ok;
}

class Rc4 {
 public:
  explicit Rc4(std::span<const uint8_t> key) {
    for (int i = 0; i < 256; ++i) s_[i] = static_cast<uint8_t>(i);
    for (int i = 0, j = 0; i < 256; ++i) {
      j = (j + s_[i] + key[i % key.size()]) & 255;
      std::swap(s_[i], s_[j]);
    }
  }
  uint8_t Next() {
    i_ = (i_ + 1) & 255;
    j_ = (j_ + s_[i_]) & 255;
    std::swap(s_[i_], s_[j_]);
    return s_[(s_[i_] + s_[j_]) & 255];
  }

 private:
  uint8_t s_[256];
  int i_ = 0, j_ = 0;
};

struct Key {
  bool cryptoApi = false;
  size_t keyBytes = 16;
  Buf base;
  bool Block(uint32_t block, Buf& key) const {
    const uint8_t le[4] = {uint8_t(block), uint8_t(block >> 8), uint8_t(block >> 16), uint8_t(block >> 24)};
    if (!Hash(cryptoApi ? BCRYPT_SHA1_ALGORITHM : BCRYPT_MD5_ALGORITHM, {base, le}, key)) return false;
    if (!cryptoApi) return true;
    if (keyBytes == 5) {
      key.resize(5);
      key.resize(16, 0);  // 40-bit keys are padded to 128 bits
    } else {
      key.resize(std::min(keyBytes, key.size()));
    }
    return true;
  }
};

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t{p[3]} << 24; }

}  // namespace

CryptResult DecryptWorkbook(std::vector<uint8_t>& s, size_t filepassAt, std::wstring_view password) {
  if (filepassAt + 6 > s.size()) return CryptResult::Damaged;
  const size_t len = U16(&s[filepassAt + 2]);
  const size_t dataAt = filepassAt + 4;
  if (len > s.size() - dataAt) return CryptResult::Damaged;
  const uint8_t* h = s.data() + dataAt;
  if (len < 6 || U16(h) != 1) return CryptResult::Unsupported;  // XOR obfuscation
  const uint16_t major = U16(h + 2), minor = U16(h + 4);
  Buf pw;
  for (wchar_t c : password) pw.push_back(uint8_t(c)), pw.push_back(uint8_t(c >> 8));

  Key key;
  std::span<const uint8_t> verifier, verifierHash;
  if (major == 1 && minor == 1) {
    if (len < 6 + 48) return CryptResult::Damaged;
    const std::span<const uint8_t> salt(h + 6, 16);
    verifier = std::span(h + 22, 16);
    verifierHash = std::span(h + 38, 16);
    Buf h0;
    if (!Hash(BCRYPT_MD5_ALGORITHM, {pw}, h0)) return CryptResult::Unsupported;
    Buf buffer;
    for (int i = 0; i < 16; ++i) {
      buffer.insert(buffer.end(), h0.begin(), h0.begin() + 5);
      buffer.insert(buffer.end(), salt.begin(), salt.end());
    }
    if (!Hash(BCRYPT_MD5_ALGORITHM, {buffer}, key.base)) return CryptResult::Unsupported;
    key.base.resize(5);
  } else if (minor == 2 && major >= 2 && major <= 4) {
    if (len < 6 + 8) return CryptResult::Damaged;
    const uint32_t flags = U32(h + 6), headerSize = U32(h + 10);
    if ((flags & 0x24) != 0x04) return CryptResult::Unsupported;
    if (headerSize < 32 || 14 + uint64_t{headerSize} + 4 + 16 + 16 + 4 + 20 > len) return CryptResult::Damaged;
    const uint8_t* eh = h + 14;
    const uint32_t algId = U32(eh + 8), algHash = U32(eh + 12);
    uint32_t keyBits = U32(eh + 16);
    if (keyBits == 0) keyBits = 40;
    if ((algId != 0 && algId != 0x6801) || (algHash != 0 && algHash != 0x8004) || keyBits < 40 || keyBits > 128 ||
        keyBits % 8)
      return CryptResult::Unsupported;
    const uint8_t* v = eh + headerSize;
    if (U32(v) != 16 || U32(v + 36) != 20) return CryptResult::Damaged;
    verifier = std::span(v + 20, 16);
    verifierHash = std::span(v + 40, 20);
    key.cryptoApi = true;
    key.keyBytes = keyBits / 8;
    if (!Hash(BCRYPT_SHA1_ALGORITHM, {std::span<const uint8_t>(v + 4, 16), pw}, key.base)) return CryptResult::Unsupported;
  } else {
    return CryptResult::Unsupported;
  }

  // The verifier and its hash: one RC4 stream with block 0's key.
  Buf k;
  if (!key.Block(0, k)) return CryptResult::Unsupported;
  Buf check(verifier.begin(), verifier.end());
  check.insert(check.end(), verifierHash.begin(), verifierHash.end());
  {
    Rc4 rc4(k);
    for (uint8_t& b : check) b ^= rc4.Next();
  }
  Buf expected;
  if (!Hash(key.cryptoApi ? BCRYPT_SHA1_ALGORITHM : BCRYPT_MD5_ALGORITHM, {std::span(check).first(16)}, expected))
    return CryptResult::Unsupported;
  if (!std::equal(expected.begin(), expected.end(), check.begin() + 16)) return CryptResult::WrongPassword;

  // Every record after FILEPASS, by its place in the stream.
  constexpr size_t kBlock = 1024;
  uint32_t block = UINT32_MAX;
  std::unique_ptr<Rc4> rc4;
  size_t streamPos = 0;  // where the key stream is
  auto decrypt = [&](size_t at, size_t n) -> bool {
    for (size_t i = 0; i < n; ++i) {
      const size_t p = at + i;
      const uint32_t b = static_cast<uint32_t>(p / kBlock);
      if (b != block || !rc4 || p < streamPos) {
        if (!key.Block(b, k)) return false;
        rc4 = std::make_unique<Rc4>(k);
        block = b;
        streamPos = size_t{b} * kBlock;
      }
      while (streamPos < p) rc4->Next(), ++streamPos;
      s[p] ^= rc4->Next();
      ++streamPos;
    }
    return true;
  };
  for (size_t p = dataAt + len; p + 4 <= s.size();) {
    const uint16_t type = U16(&s[p]);
    const size_t n = U16(&s[p + 2]);
    if (n > s.size() - p - 4) break;
    switch (type) {
      case 0x0809: case 0x002F: case 0x0194: case 0x0195: case 0x00E1: case 0x0196: case 0x0138: break;  // clear
      case 0x0085:  // BOUNDSHEET: lbPlyPos stays clear
        if (n > 4 && !decrypt(p + 8, n - 4)) return CryptResult::Unsupported;
        break;
      default:
        if (!decrypt(p + 4, n)) return CryptResult::Unsupported;
    }
    p += 4 + n;
  }
  return CryptResult::Ok;
}

}  // namespace fw::sheet::xls::detail
