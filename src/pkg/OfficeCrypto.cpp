#include "pkg/OfficeCrypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <pugixml.hpp>

#include "pkg/CompoundFile.h"

namespace fw::pkg::crypto {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr char kPasswordUri[] = "http://schemas.microsoft.com/office/2006/keyEncryptor/password";
constexpr uint32_t kSegment = 4096;  // the package is encrypted in segments, each with its own IV
constexpr uint32_t kBlock = 16;      // AES
constexpr uint32_t kMaxSpins = 10000000;
// The block keys of [MS-OFFCRYPTO] 2.3.4.13 and 2.3.4.14.
constexpr uint8_t kVerifierInputBlock[] = {0xfe, 0xa7, 0xd2, 0x76, 0x3b, 0x4b, 0x9e, 0x79};
constexpr uint8_t kVerifierHashBlock[] = {0xd7, 0xaa, 0x0f, 0x6d, 0x30, 0x61, 0x34, 0x4e};
constexpr uint8_t kKeyValueBlock[] = {0x14, 0x6e, 0x0b, 0xe7, 0xab, 0xac, 0xd0, 0xd6};
constexpr uint8_t kHmacKeyBlock[] = {0x5f, 0xb2, 0xad, 0x01, 0x0c, 0xb9, 0xe1, 0xf6};
constexpr uint8_t kHmacValueBlock[] = {0xa0, 0x67, 0x7f, 0x02, 0xb2, 0x2c, 0x84, 0x33};

const wchar_t* HashId(std::string_view name) {
  if (name == "SHA1") return BCRYPT_SHA1_ALGORITHM;
  if (name == "SHA256") return BCRYPT_SHA256_ALGORITHM;
  if (name == "SHA384") return BCRYPT_SHA384_ALGORITHM;
  if (name == "SHA512") return BCRYPT_SHA512_ALGORITHM;
  if (name == "MD5") return BCRYPT_MD5_ALGORITHM;  // RC4 in .doc files
  return nullptr;
}

uint32_t HashSize(std::string_view name) {
  return name == "SHA1" ? 20 : name == "SHA256" ? 32 : name == "SHA384" ? 48 : name == "SHA512" ? 64 : name == "MD5" ? 16 : 0;
}

void Wipe(Bytes& b) {
  if (!b.empty()) SecureZeroMemory(b.data(), b.size());
}

Bytes Le32(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }

std::span<const uint8_t> PasswordBytes(std::wstring_view password) {
  return {reinterpret_cast<const uint8_t*>(password.data()), password.size() * sizeof(wchar_t)};  // UTF-16LE
}

// A hash or HMAC through CNG, reusable: each Finish starts the next one.
class Hasher {
 public:
  explicit Hasher(std::string_view name, std::span<const uint8_t> hmacKey = {}) {
    const wchar_t* id = HashId(name);
    ok_ = id && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, id, nullptr, hmacKey.empty() ? 0 : BCRYPT_ALG_HANDLE_HMAC_FLAG)) &&
          BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash_, nullptr, 0, const_cast<uint8_t*>(hmacKey.data()),
                                          ULONG(hmacKey.size()), BCRYPT_HASH_REUSABLE_FLAG));
    size_ = HashSize(name);
  }
  ~Hasher() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;

  bool Ok() const { return ok_; }
  Hasher& Add(std::span<const uint8_t> data) {
    if (ok_ && !data.empty()) ok_ = BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<uint8_t*>(data.data()), ULONG(data.size()), 0));
    return *this;
  }
  Bytes Finish() {
    Bytes out(size_);
    if (ok_) ok_ = BCRYPT_SUCCESS(BCryptFinishHash(hash_, out.data(), ULONG(out.size()), 0));
    return out;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  uint32_t size_ = 0;
  bool ok_ = false;
};

// AES on whole blocks: CBC with an IV, or ECB (standard encryption) without one.
class Aes {
 public:
  Aes(std::span<const uint8_t> key, bool cbc) {
    ok_ = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
          BCRYPT_SUCCESS(BCryptSetProperty(alg_, BCRYPT_CHAINING_MODE,
                                           (PUCHAR)(cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB),
                                           ULONG(sizeof(wchar_t) * (wcslen(cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB) + 1)), 0)) &&
          BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg_, &key_, nullptr, 0, const_cast<uint8_t*>(key.data()), ULONG(key.size()), 0));
  }
  ~Aes() {
    if (key_) BCryptDestroyKey(key_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  Aes(const Aes&) = delete;
  Aes& operator=(const Aes&) = delete;

  // `in` is a whole number of blocks; out gets as many bytes.
  bool Run(bool encrypt, std::span<const uint8_t> iv, std::span<const uint8_t> in, uint8_t* out) {
    if (!ok_ || in.size() % kBlock) return false;
    if (in.empty()) return true;
    uint8_t chain[kBlock] = {};  // CNG changes the IV it is given
    if (!iv.empty()) std::memcpy(chain, iv.data(), std::min<size_t>(iv.size(), kBlock));
    ULONG done = 0;
    auto fn = encrypt ? BCryptEncrypt : BCryptDecrypt;
    return BCRYPT_SUCCESS(fn(key_, const_cast<uint8_t*>(in.data()), ULONG(in.size()), nullptr, iv.empty() ? nullptr : chain,
                             iv.empty() ? 0 : kBlock, out, ULONG(in.size()), &done, 0)) &&
           done == in.size();
  }
  Bytes Run(bool encrypt, std::span<const uint8_t> iv, std::span<const uint8_t> in) {
    Bytes out(in.size());
    if (!Run(encrypt, iv, in, out.data())) out.clear();
    return out;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_KEY_HANDLE key_ = nullptr;
  bool ok_ = false;
};

Bytes Random(size_t n) {
  Bytes b(n);
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b.data(), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) b.clear();
  return b;
}

// Cut to `size`, or padded with `pad` ([MS-OFFCRYPTO] 2.3.4.12, for keys and IVs).
Bytes Fit(Bytes b, size_t size, uint8_t pad = 0x36) {
  b.resize(size, pad);
  return b;
}

Bytes PadToBlock(std::span<const uint8_t> b) {
  Bytes out(b.begin(), b.end());
  out.resize((out.size() + kBlock - 1) / kBlock * kBlock);
  return out;
}

std::optional<Bytes> FromBase64(const char* s) {
  DWORD n = 0;
  const DWORD len = DWORD(strlen(s));
  if (!len || !CryptStringToBinaryA(s, len, CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return std::nullopt;
  Bytes b(n);
  if (!CryptStringToBinaryA(s, len, CRYPT_STRING_BASE64, b.data(), &n, nullptr, nullptr)) return std::nullopt;
  b.resize(n);
  return b;
}

std::string ToBase64(std::span<const uint8_t> b) {
  DWORD n = 0;
  const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
  if (b.empty() || !CryptBinaryToStringA(b.data(), DWORD(b.size()), flags, nullptr, &n)) return {};
  std::string s(n, '\0');
  if (!CryptBinaryToStringA(b.data(), DWORD(b.size()), flags, s.data(), &n)) return {};
  s.resize(n);
  return s;
}

// H(salt + password), then spinCount times H(iterator + previous) ([MS-OFFCRYPTO] 2.3.4.11).
Bytes PasswordHash(std::string_view hash, std::span<const uint8_t> salt, std::wstring_view password, uint32_t spins) {
  Hasher h(hash);
  Bytes value = h.Add(salt).Add(PasswordBytes(password)).Finish();
  for (uint32_t i = 0; i < spins && h.Ok(); ++i) {
    const uint8_t it[4] = {uint8_t(i), uint8_t(i >> 8), uint8_t(i >> 16), uint8_t(i >> 24)};
    Bytes next = h.Add(it).Add(value).Finish();
    Wipe(value);
    value = std::move(next);
  }
  if (!h.Ok()) value.clear();
  return value;
}

Bytes BlockKey(std::string_view hash, std::span<const uint8_t> passwordHash, std::span<const uint8_t> block, size_t bytes) {
  Hasher h(hash);
  Bytes k = h.Add(passwordHash).Add(block).Finish();
  return h.Ok() ? Fit(std::move(k), bytes) : Bytes();
}

Bytes Iv(std::string_view hash, std::span<const uint8_t> salt, std::span<const uint8_t> block) {
  Hasher h(hash);
  Bytes iv = h.Add(salt).Add(block).Finish();
  return h.Ok() ? Fit(std::move(iv), kBlock) : Bytes();
}

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t U32(const uint8_t* p) { return uint32_t(U16(p)) | uint32_t(U16(p + 2)) << 16; }
uint64_t U64(const uint8_t* p) { return U32(p) | uint64_t(U32(p + 4)) << 32; }

std::string_view LocalName(const char* name) {
  std::string_view n(name);
  const size_t colon = n.find(':');
  return colon == std::string_view::npos ? n : n.substr(colon + 1);
}

struct AgileInfo {
  // keyData
  std::string dataHash;
  Bytes dataSalt;
  // The password key encryptor
  std::string passwordHash;
  uint32_t spins = 0, keyBytes = 0;
  Bytes salt, verifierInput, verifierHash, encryptedSecret;
};

// The cipher settings Featherweight reads: AES in CBC mode, with a hash it knows.
bool Supported(const pugi::xml_node& n) {
  const std::string_view hash = n.attribute("hashAlgorithm").as_string();
  const uint32_t bits = n.attribute("keyBits").as_uint();
  return std::string_view(n.attribute("cipherAlgorithm").as_string()) == "AES" &&
         std::string_view(n.attribute("cipherChaining").as_string()) == "ChainingModeCBC" &&
         n.attribute("blockSize").as_uint() == kBlock && (bits == 128 || bits == 192 || bits == 256) && HashId(hash) &&
         n.attribute("hashSize").as_uint() == HashSize(hash);
}

Result ReadAgile(std::span<const uint8_t> xml, AgileInfo& info) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8)) return Result::Damaged;
  const pugi::xml_node root = doc.document_element();
  if (LocalName(root.name()) != "encryption") return Result::Damaged;
  pugi::xml_node keyData, encryptedKey;
  for (pugi::xml_node n : root.children()) {
    if (LocalName(n.name()) == "keyData") keyData = n;
    if (LocalName(n.name()) != "keyEncryptors") continue;
    for (pugi::xml_node e : n.children())
      if (std::string_view(e.attribute("uri").as_string()) == kPasswordUri)
        for (pugi::xml_node k : e.children())
          if (LocalName(k.name()) == "encryptedKey") encryptedKey = k;
  }
  if (!keyData || !encryptedKey) return Result::Unsupported;  // a certificate's, say
  if (!Supported(keyData) || !Supported(encryptedKey)) return Result::Unsupported;
  info.dataHash = keyData.attribute("hashAlgorithm").as_string();
  info.passwordHash = encryptedKey.attribute("hashAlgorithm").as_string();
  info.spins = encryptedKey.attribute("spinCount").as_uint();
  info.keyBytes = encryptedKey.attribute("keyBits").as_uint() / 8;
  if (info.spins > kMaxSpins) return Result::Unsupported;
  const auto dataSalt = FromBase64(keyData.attribute("saltValue").as_string());
  const auto salt = FromBase64(encryptedKey.attribute("saltValue").as_string());
  const auto input = FromBase64(encryptedKey.attribute("encryptedVerifierHashInput").as_string());
  const auto hash = FromBase64(encryptedKey.attribute("encryptedVerifierHashValue").as_string());
  const auto secret = FromBase64(encryptedKey.attribute("encryptedKeyValue").as_string());
  if (!dataSalt || !salt || !input || !hash || !secret) return Result::Damaged;
  if (input->size() % kBlock || hash->size() % kBlock || secret->size() % kBlock || secret->size() < info.keyBytes ||
      hash->size() < HashSize(info.passwordHash))
    return Result::Damaged;
  info.dataSalt = *dataSalt;
  info.salt = *salt;
  info.verifierInput = *input;
  info.verifierHash = *hash;
  info.encryptedSecret = *secret;
  return Result::Ok;
}

// The password key encryptor's three keys, from the password.
struct PasswordKeys {
  Bytes input, hash, secret;
  ~PasswordKeys() { Wipe(input), Wipe(hash), Wipe(secret); }
};
bool MakePasswordKeys(std::string_view hash, std::span<const uint8_t> salt, std::wstring_view password, uint32_t spins,
                      size_t keyBytes, PasswordKeys& keys) {
  Bytes h = PasswordHash(hash, salt, password, spins);
  if (h.empty()) return false;
  keys.input = BlockKey(hash, h, kVerifierInputBlock, keyBytes);
  keys.hash = BlockKey(hash, h, kVerifierHashBlock, keyBytes);
  keys.secret = BlockKey(hash, h, kKeyValueBlock, keyBytes);
  Wipe(h);
  return !keys.input.empty() && !keys.hash.empty() && !keys.secret.empty();
}

Result DecryptAgile(std::span<const uint8_t> xml, std::span<const uint8_t> encrypted, std::wstring_view password,
                    Bytes& package, Key& key) {
  AgileInfo info;
  if (Result r = ReadAgile(xml, info); r != Result::Ok) return r;
  PasswordKeys keys;
  if (!MakePasswordKeys(info.passwordHash, info.salt, password, info.spins, info.keyBytes, keys)) return Result::Unsupported;
  const Bytes iv = Fit(info.salt, kBlock);
  Bytes input = Aes(keys.input, true).Run(false, iv, info.verifierInput);
  Bytes hash = Aes(keys.hash, true).Run(false, iv, info.verifierHash);
  if (input.empty() || hash.empty()) return Result::Unsupported;
  input.resize(std::min(input.size(), info.salt.size()));  // the verifier is as long as the salt
  hash.resize(HashSize(info.passwordHash));
  Hasher check(info.passwordHash);
  const bool right = check.Add(input).Finish() == hash;
  Wipe(input), Wipe(hash);
  if (!right) return Result::WrongPassword;
  Bytes secret = Aes(keys.secret, true).Run(false, iv, info.encryptedSecret);
  if (secret.empty()) return Result::Unsupported;
  secret.resize(info.keyBytes);

  // The package: its size, then 4096-byte segments.
  if (encrypted.size() < 8) return Result::Damaged;
  const uint64_t size = U64(encrypted.data());
  const std::span<const uint8_t> data = encrypted.subspan(8);
  const uint64_t needed = (size + kBlock - 1) / kBlock * kBlock;
  if (size > data.size() || needed > data.size()) {
    Wipe(secret);
    return Result::Damaged;
  }
  package.assign(size_t(needed), 0);
  Aes aes(secret, true);
  for (uint64_t at = 0, i = 0; at < needed; at += kSegment, ++i) {
    const size_t n = size_t(std::min<uint64_t>(kSegment, needed - at));
    const Bytes segmentIv = Iv(info.dataHash, info.dataSalt, Le32(uint32_t(i)));
    if (segmentIv.empty() || !aes.Run(false, segmentIv, data.subspan(size_t(at), n), package.data() + at)) {
      Wipe(secret);
      package.clear();
      return Result::Unsupported;
    }
  }
  package.resize(size_t(size));

  // Saving reuses the package key and the password's copy of it.
  key.secret = std::move(secret);
  key.dataHash = info.dataHash;
  key.passwordHash = info.passwordHash;
  key.spinCount = info.spins;
  key.salt = info.salt;
  key.verifierInput = info.verifierInput;
  key.verifierHash = info.verifierHash;
  key.encryptedSecret = info.encryptedSecret;
  return Result::Ok;
}

// Standard encryption: AES in ECB mode, SHA-1 with 50,000 spins ([MS-OFFCRYPTO] 2.3.4.5-2.3.4.9).
Result DecryptStandard(std::span<const uint8_t> info, std::span<const uint8_t> encrypted, std::wstring_view password,
                       Bytes& package) {
  if (info.size() < 12) return Result::Damaged;
  const uint32_t flags = U32(&info[4]), headerSize = U32(&info[8]);
  constexpr uint32_t kCryptoApi = 0x04, kAes = 0x20;
  if ((flags & (kCryptoApi | kAes)) != (kCryptoApi | kAes)) return Result::Unsupported;  // RC4, or not CryptoAPI
  if (headerSize < 32 || headerSize > info.size() - 12) return Result::Damaged;
  const uint8_t* header = &info[12];
  const uint32_t algId = U32(header + 8), algHash = U32(header + 12), keyBits = U32(header + 16);
  const bool aes = algId == 0 || (algId == 0x660E && keyBits == 128) || (algId == 0x660F && keyBits == 192) ||
                   (algId == 0x6610 && keyBits == 256);
  if (!aes || (algHash != 0 && algHash != 0x8004) || (keyBits != 128 && keyBits != 192 && keyBits != 256))
    return Result::Unsupported;
  const size_t v = 12 + headerSize;
  if (info.size() < v + 4 + 16 + 16 + 4 + 32 || U32(&info[v]) != 16) return Result::Damaged;
  const std::span<const uint8_t> salt = info.subspan(v + 4, 16);
  const std::span<const uint8_t> verifier = info.subspan(v + 20, 16);
  const std::span<const uint8_t> verifierHash = info.subspan(v + 40, 32);

  Bytes h = PasswordHash("SHA1", salt, password, 50000);
  Hasher sha1("SHA1");
  Bytes last = sha1.Add(h).Add(Le32(0)).Finish();
  Wipe(h);
  if (last.empty() || !sha1.Ok()) return Result::Unsupported;
  Bytes x(40);
  for (int part = 0; part < 2; ++part) {
    uint8_t buf[64];
    std::memset(buf, part ? 0x5C : 0x36, sizeof(buf));
    for (size_t i = 0; i < last.size(); ++i) buf[i] ^= last[i];
    const Bytes xi = sha1.Add(buf).Finish();
    std::copy(xi.begin(), xi.end(), x.begin() + 20 * part);
    SecureZeroMemory(buf, sizeof(buf));
  }
  Wipe(last);
  Bytes key(x.begin(), x.begin() + keyBits / 8);
  Wipe(x);
  Aes ecb(key, false);
  Wipe(key);
  Bytes plain = ecb.Run(false, {}, verifier);
  Bytes plainHash = ecb.Run(false, {}, verifierHash);
  if (plain.empty() || plainHash.empty()) return Result::Unsupported;
  plainHash.resize(20);
  const bool right = sha1.Add(plain).Finish() == plainHash;
  Wipe(plain), Wipe(plainHash);
  if (!right) return Result::WrongPassword;

  if (encrypted.size() < 8) return Result::Damaged;
  const uint64_t size = U64(encrypted.data());
  const uint64_t needed = (size + kBlock - 1) / kBlock * kBlock;
  if (needed > encrypted.size() - 8) return Result::Damaged;
  package.assign(size_t(needed), 0);
  if (!ecb.Run(false, {}, encrypted.subspan(8, size_t(needed)), package.data())) {
    package.clear();
    return Result::Unsupported;
  }
  package.resize(size_t(size));
  return Result::Ok;
}

// The \x06DataSpaces storage that says EncryptedPackage is encrypted ([MS-OFFCRYPTO] 2.1, 2.2):
// streams as Word writes them.
void PutLp(Bytes& out, std::u16string_view s) {  // UNICODE-LP-P4
  const Bytes len = Le32(uint32_t(s.size() * 2));
  out.insert(out.end(), len.begin(), len.end());
  for (char16_t c : s) out.push_back(uint8_t(c)), out.push_back(uint8_t(c >> 8));
  while (out.size() % 4) out.push_back(0);
}
void Put32(Bytes& out, uint32_t v) {
  const Bytes b = Le32(v);
  out.insert(out.end(), b.begin(), b.end());
}

Bytes VersionStream() {
  Bytes b;
  PutLp(b, u"Microsoft.Container.DataSpaces");
  for (int i = 0; i < 3; ++i) Put32(b, 1);  // reader, updater and writer version 1.0
  return b;
}
Bytes DataSpaceMap() {
  Bytes entry;
  Put32(entry, 1);  // one reference component
  Put32(entry, 0);  // a stream
  PutLp(entry, u"EncryptedPackage");
  PutLp(entry, u"StrongEncryptionDataSpace");
  Bytes b;
  Put32(b, 8);  // header length
  Put32(b, 1);  // entries
  Put32(b, uint32_t(entry.size() + 4));
  b.insert(b.end(), entry.begin(), entry.end());
  return b;
}
Bytes DataSpaceDefinition() {
  Bytes b;
  Put32(b, 8);  // header length
  Put32(b, 1);  // transforms
  PutLp(b, u"StrongEncryptionTransform");
  return b;
}
Bytes TransformInfo() {
  Bytes b;
  const std::u16string_view id = u"{FF9A3F03-56EF-4613-BDD5-5A41C1D07246}";
  Put32(b, uint32_t(12 + id.size() * 2));  // the header's length before the transform's name
  Put32(b, 1);                             // a transform
  PutLp(b, id);
  PutLp(b, u"Microsoft.Container.EncryptionTransform");
  for (int i = 0; i < 3; ++i) Put32(b, 1);  // versions 1.0
  Put32(b, 0);  // no encryption name
  Put32(b, 0);  // block size
  Put32(b, 0);  // cipher mode
  Put32(b, 4);  // reserved
  return b;
}

// ---- .doc: RC4 in 512-byte blocks, each with a key of its own ([MS-OFFCRYPTO] 2.3.6, 2.3.5).

constexpr size_t kRc4Block = 512;
constexpr size_t kFibBase = 68;  // the start of WordDocument that isn't encrypted

bool Rc4(std::span<const uint8_t> key, std::span<const uint8_t> in, uint8_t* out) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE k = nullptr;
  ULONG done = 0;
  const bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RC4_ALGORITHM, nullptr, 0)) &&
                  BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, const_cast<uint8_t*>(key.data()), ULONG(key.size()), 0)) &&
                  BCRYPT_SUCCESS(BCryptDecrypt(k, const_cast<uint8_t*>(in.data()), ULONG(in.size()), nullptr, nullptr, 0, out,
                                               ULONG(in.size()), &done, 0)) &&
                  done == in.size();
  if (k) BCryptDestroyKey(k);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

// How each block's key is made from the password.
struct DocKey {
  bool cryptoApi = false;  // SHA-1 (RC4 CryptoAPI) or MD5 (RC4)
  size_t keyBytes = 16;
  Bytes base;  // CryptoAPI: H(salt + password). RC4: the truncated hash of the salted password.
  ~DocKey() { Wipe(base); }
  Bytes Block(uint32_t block) const {
    Hasher h(cryptoApi ? "SHA1" : "MD5");
    Bytes key = h.Add(base).Add(Le32(block)).Finish();
    if (!h.Ok()) return {};
    if (!cryptoApi) return key;  // all 16 bytes of the MD5
    if (keyBytes == 5) return Fit(Fit(std::move(key), 5, 0), 16, 0);  // 40-bit: padded with zeros to 128
    return Fit(std::move(key), keyBytes, 0);
  }
};

bool DecryptBlocks(const DocKey& key, Bytes& stream) {
  for (size_t at = 0; at < stream.size(); at += kRc4Block) {
    const Bytes k = key.Block(uint32_t(at / kRc4Block));
    const size_t n = std::min(kRc4Block, stream.size() - at);
    if (k.empty() || !Rc4(k, std::span(stream).subspan(at, n), stream.data() + at)) return false;
  }
  return true;
}

std::string Attr(const char* name, const std::string& value) { return std::string(" ") + name + "=\"" + value + "\""; }
std::string CipherAttrs(size_t saltBytes, size_t keyBytes, const std::string& hash) {
  return Attr("saltSize", std::to_string(saltBytes)) + Attr("blockSize", std::to_string(kBlock)) +
         Attr("keyBits", std::to_string(keyBytes * 8)) + Attr("hashSize", std::to_string(HashSize(hash))) +
         Attr("cipherAlgorithm", "AES") + Attr("cipherChaining", "ChainingModeCBC") + Attr("hashAlgorithm", hash);
}

}  // namespace

Key::~Key() {
  Wipe(secret);
}

bool IsEncryptedPackage(const CompoundFile& file) {
  const auto streams = file.Streams();
  auto has = [&](std::string_view name) {
    return std::any_of(streams.begin(), streams.end(), [&](const std::string& s) { return _stricmp(s.c_str(), name.data()) == 0; });
  };
  return has("EncryptionInfo") && has("EncryptedPackage");
}

Result Decrypt(const CompoundFile& file, std::wstring_view password, std::vector<uint8_t>& package, Key& key) {
  package.clear();
  const auto info = file.Stream("EncryptionInfo");
  const auto encrypted = file.Stream("EncryptedPackage");
  if (!info || !encrypted || info->size() < 8) return Result::Damaged;
  const uint16_t major = U16(info->data()), minor = U16(info->data() + 2);
  if (major == 4 && minor == 4) {
    if (info->size() > (1u << 20)) return Result::Unsupported;
    return DecryptAgile(std::span(*info).subspan(8), *encrypted, password, package, key);
  }
  if (minor == 2 && (major == 2 || major == 3 || major == 4)) {
    const Result r = DecryptStandard(*info, *encrypted, password, package);
    if (r == Result::Ok) key = NewKey(password);  // saved with agile encryption, as Word does
    if (r == Result::Ok && key.Empty()) return Result::Unsupported;
    return r;
  }
  return Result::Unsupported;  // extensible encryption, or a version that doesn't exist yet
}

Key NewKey(std::wstring_view password) {
  Key key;
  key.dataHash = key.passwordHash = "SHA512";
  key.spinCount = 100000;
  key.salt = Random(16);
  key.secret = Random(32);
  Bytes input = Random(16);
  PasswordKeys keys;
  if (key.salt.empty() || key.secret.empty() || input.empty() ||
      !MakePasswordKeys(key.passwordHash, key.salt, password, key.spinCount, key.secret.size(), keys))
    return {};
  Hasher h(key.passwordHash);
  Bytes inputHash = h.Add(input).Finish();
  key.verifierInput = Aes(keys.input, true).Run(true, key.salt, input);
  key.verifierHash = Aes(keys.hash, true).Run(true, key.salt, inputHash);
  key.encryptedSecret = Aes(keys.secret, true).Run(true, key.salt, key.secret);
  Wipe(input), Wipe(inputHash);
  if (!h.Ok() || key.verifierInput.empty() || key.verifierHash.empty() || key.encryptedSecret.empty()) return {};
  return key;
}

std::vector<uint8_t> Encrypt(std::span<const uint8_t> package, const Key& key) {
  if (key.Empty() || !HashId(key.dataHash) || !HashId(key.passwordHash)) return {};
  const Bytes dataSalt = Random(16);
  if (dataSalt.empty()) return {};

  // EncryptedPackage: the size, then each 4096-byte segment with its own IV.
  Bytes encrypted(8);
  for (int i = 0; i < 8; ++i) encrypted[i] = uint8_t(uint64_t(package.size()) >> (8 * i));
  encrypted.resize(8 + (package.size() + kBlock - 1) / kBlock * kBlock);
  Aes aes(key.secret, true);
  for (size_t at = 0, i = 0; at < package.size(); at += kSegment, ++i) {
    const Bytes segment = PadToBlock(package.subspan(at, std::min<size_t>(kSegment, package.size() - at)));
    const Bytes iv = Iv(key.dataHash, dataSalt, Le32(uint32_t(i)));
    if (iv.empty() || !aes.Run(true, iv, segment, encrypted.data() + 8 + at)) return {};
  }

  // Data integrity: an HMAC of EncryptedPackage, its key and value encrypted with the package key.
  const Bytes hmacKey = Random(HashSize(key.dataHash));
  if (hmacKey.empty()) return {};
  Hasher hmac(key.dataHash, hmacKey);
  const Bytes hmacValue = hmac.Add(encrypted).Finish();
  const Bytes keyIv = Iv(key.dataHash, dataSalt, kHmacKeyBlock), valueIv = Iv(key.dataHash, dataSalt, kHmacValueBlock);
  const Bytes encryptedHmacKey = aes.Run(true, keyIv, PadToBlock(hmacKey));
  const Bytes encryptedHmacValue = aes.Run(true, valueIv, PadToBlock(hmacValue));
  if (!hmac.Ok() || encryptedHmacKey.empty() || encryptedHmacValue.empty()) return {};

  std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
      "<encryption xmlns=\"http://schemas.microsoft.com/office/2006/encryption\""
      " xmlns:p=\"http://schemas.microsoft.com/office/2006/keyEncryptor/password\""
      " xmlns:c=\"http://schemas.microsoft.com/office/2006/keyEncryptor/certificate\">";
  xml += "<keyData" + CipherAttrs(dataSalt.size(), key.secret.size(), key.dataHash) + Attr("saltValue", ToBase64(dataSalt)) + "/>";
  xml += "<dataIntegrity" + Attr("encryptedHmacKey", ToBase64(encryptedHmacKey)) +
         Attr("encryptedHmacValue", ToBase64(encryptedHmacValue)) + "/>";
  xml += "<keyEncryptors><keyEncryptor uri=\"" + std::string(kPasswordUri) + "\"><p:encryptedKey" +
         Attr("spinCount", std::to_string(key.spinCount)) +
         CipherAttrs(key.salt.size(), key.secret.size(), key.passwordHash) + Attr("saltValue", ToBase64(key.salt)) +
         Attr("encryptedVerifierHashInput", ToBase64(key.verifierInput)) +
         Attr("encryptedVerifierHashValue", ToBase64(key.verifierHash)) +
         Attr("encryptedKeyValue", ToBase64(key.encryptedSecret)) + "/></keyEncryptor></keyEncryptors></encryption>";
  Bytes info = {4, 0, 4, 0, 0x40, 0, 0, 0};  // version 4.4, agile
  info.insert(info.end(), xml.begin(), xml.end());

  const Bytes version = VersionStream(), map = DataSpaceMap(), definition = DataSpaceDefinition(), transform = TransformInfo();
  return WriteCompoundFile({{"\x06" "DataSpaces/Version", version},
                            {"\x06" "DataSpaces/DataSpaceMap", map},
                            {"\x06" "DataSpaces/DataSpaceInfo/StrongEncryptionDataSpace", definition},
                            {"\x06" "DataSpaces/TransformInfo/StrongEncryptionTransform/\x06Primary", transform},
                            {"EncryptionInfo", info},
                            {"EncryptedPackage", encrypted}});
}

bool IsEncryptedDoc(const CompoundFile& file) {
  const auto word = file.Stream("WordDocument");
  return word && word->size() >= kFibBase && (U16(&(*word)[0x0A]) & 0x0100);
}

Result DecryptDoc(const CompoundFile& file, std::wstring_view password, DocStreams& out) {
  out = {};
  auto word = file.Stream("WordDocument");
  if (!word || word->size() < kFibBase) return Result::Damaged;
  const uint16_t flags = U16(&(*word)[0x0A]);
  if (!(flags & 0x0100)) return Result::Damaged;      // not encrypted
  if (flags & 0x8000) return Result::Unsupported;     // XOR obfuscation
  out.table = flags & 0x0200 ? "1Table" : "0Table";
  auto table = file.Stream(out.table);
  const uint32_t lKey = U32(&(*word)[0x0E]);  // the encryption header's size
  if (!table || lKey < 52 || lKey > table->size()) return Result::Damaged;
  const uint8_t* h = table->data();
  const uint16_t major = U16(h), minor = U16(h + 2);

  DocKey key;
  std::span<const uint8_t> salt, verifier, verifierHash;
  if (major == 1 && minor == 1) {
    // RC4: MD5 of the password, salted 16 times over.
    salt = std::span(h + 4, 16);
    verifier = std::span(h + 20, 16);
    verifierHash = std::span(h + 36, 16);
    Hasher md5("MD5");
    Bytes h0 = md5.Add(PasswordBytes(password)).Finish();
    Bytes buffer;
    for (int i = 0; i < 16; ++i) {
      buffer.insert(buffer.end(), h0.begin(), h0.begin() + 5);
      buffer.insert(buffer.end(), salt.begin(), salt.end());
    }
    key.base = Fit(md5.Add(buffer).Finish(), 5);
    Wipe(h0), Wipe(buffer);
    if (!md5.Ok()) return Result::Unsupported;
  } else if (minor == 2 && major >= 2 && major <= 4) {
    // RC4 CryptoAPI: SHA-1 of the salted password.
    const uint32_t eflags = U32(h + 4), headerSize = U32(h + 8);
    if ((eflags & 0x24) != 0x04) return Result::Unsupported;  // CryptoAPI, not AES
    if (headerSize < 32 || 12 + uint64_t(headerSize) + 4 + 16 + 16 + 4 + 20 > lKey) return Result::Damaged;
    const uint8_t* eh = h + 12;
    const uint32_t algId = U32(eh + 8), algHash = U32(eh + 12);
    uint32_t keyBits = U32(eh + 16);
    if (keyBits == 0) keyBits = 40;
    if ((algId != 0 && algId != 0x6801) || (algHash != 0 && algHash != 0x8004) || keyBits < 40 || keyBits > 128 || keyBits % 8)
      return Result::Unsupported;
    const uint8_t* v = eh + headerSize;
    if (U32(v) != 16 || U32(v + 36) != 20) return Result::Damaged;
    salt = std::span(v + 4, 16);
    verifier = std::span(v + 20, 16);
    verifierHash = std::span(v + 40, 20);
    key.cryptoApi = true;
    key.keyBytes = keyBits / 8;
    Hasher sha1("SHA1");
    key.base = sha1.Add(salt).Add(PasswordBytes(password)).Finish();
    if (!sha1.Ok()) return Result::Unsupported;
  } else {
    return Result::Unsupported;
  }

  // The verifier and its hash, one RC4 stream with block 0's key.
  Bytes check(verifier.begin(), verifier.end());
  check.insert(check.end(), verifierHash.begin(), verifierHash.end());
  const Bytes k0 = key.Block(0);
  if (k0.empty() || !Rc4(k0, Bytes(check), check.data())) return Result::Unsupported;
  Hasher h2(key.cryptoApi ? "SHA1" : "MD5");
  const Bytes expected = h2.Add(std::span(check).first(16)).Finish();
  const bool right = std::equal(expected.begin(), expected.end(), check.begin() + 16);
  Wipe(check);
  if (!right) return Result::WrongPassword;

  // Whole streams, block numbers from their start; then the parts that weren't encrypted back.
  Bytes wordPlain = *word, tablePlain = *table;
  if (!DecryptBlocks(key, wordPlain) || !DecryptBlocks(key, tablePlain)) return Result::Unsupported;
  std::copy(word->begin(), word->begin() + kFibBase, wordPlain.begin());
  std::copy(table->begin(), table->begin() + lKey, tablePlain.begin());
  wordPlain[0x0B] &= ~0x01;                          // fEncrypted
  std::fill(wordPlain.begin() + 0x0E, wordPlain.begin() + 0x12, uint8_t(0));  // lKey
  if (auto data = file.Stream("Data")) {
    out.data = std::move(*data);
    if (!DecryptBlocks(key, out.data)) return Result::Unsupported;
  }
  out.wordDocument = std::move(wordPlain);
  out.tableStream = std::move(tablePlain);
  return Result::Ok;
}

}  // namespace fw::pkg::crypto
